#include "tetherkitnext/usb/device.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"

namespace tetherkitnext::usb {
namespace {

/// CDC functional descriptor type (bDescriptorType = CS_INTERFACE).
constexpr std::uint8_t kCsInterfaceDescriptorType = 0x24;
/// CDC Union functional descriptor subtype.
constexpr std::uint8_t kUnionFunctionalSubtype = 0x06;
/// CDC ACM functional descriptor subtype (used to recognize real cdc-acm modems).
constexpr std::uint8_t kAcmFunctionalSubtype = 0x02;

/// The hard-coded interface numbers of the Android quirk (consistent with Linux's android_rndis_quirk).
constexpr std::uint8_t kAndroidQuirkControlInterface = 0;
constexpr std::uint8_t kAndroidQuirkDataInterface = 1;

/// Gets the class/subclass/protocol triple of an interface.
[[nodiscard]] rndis::InterfaceSignature SignatureOf(
    const ::libusb_interface_descriptor& descriptor) {
  return rndis::InterfaceSignature{descriptor.bInterfaceClass, descriptor.bInterfaceSubClass,
                                   descriptor.bInterfaceProtocol};
}

/// Finds a CDC functional descriptor in the interface's extra descriptors.
///
/// Returning nullptr means not found. `out_length` outputs the total length of that descriptor.
[[nodiscard]] const std::uint8_t* FindCdcFunctional(const ::libusb_interface_descriptor& descriptor,
                                                    std::uint8_t subtype,
                                                    std::uint8_t& out_length) {
  const std::uint8_t* cursor = descriptor.extra;
  int remaining = descriptor.extra_length;

  // The generic format of a CDC functional descriptor chain: [bLength][bDescriptorType][bDescriptorSubtype][...]
  while (remaining >= 3) {
    const std::uint8_t length = cursor[0];
    if (length < 3 || length > remaining) {
      break;  // the descriptor chain is corrupted; stop parsing
    }
    if (cursor[1] == kCsInterfaceDescriptorType && cursor[2] == subtype) {
      out_length = length;
      return cursor;
    }
    cursor += length;
    remaining -= length;
  }
  out_length = 0;
  return nullptr;
}

/// Judges whether it is a "fake RNDIS" -- a real CDC ACM modem.
///
/// Criterion: class == 0x02 with an ACM functional descriptor carrying a **non-zero** bmCapabilities.
/// **This check can only apply to class == 0x02** -- the RNDIS function of the wireless class (0xE0)
/// repurposes the bmCapabilities field for its own use, and applying this rule to it would misjudge real RNDIS.
[[nodiscard]] bool LooksLikeRealAcmModem(const ::libusb_interface_descriptor& descriptor) {
  if (descriptor.bInterfaceClass != 0x02) {
    return false;
  }
  std::uint8_t length = 0;
  const std::uint8_t* acm = FindCdcFunctional(descriptor, kAcmFunctionalSubtype, length);
  if (acm == nullptr || length < 4) {
    return false;
  }
  // Layout: [bLength][CS_INTERFACE][ACM subtype][bmCapabilities]
  return acm[3] != 0;
}

/// Gets the first slave interface number from a CDC Union functional descriptor.
///
/// Layout: [bLength][CS_INTERFACE][UNION subtype][bMasterInterface][bSlaveInterface0]...
[[nodiscard]] bool TryReadUnionSlaveInterface(const ::libusb_interface_descriptor& descriptor,
                                             std::uint8_t& out_slave) {
  std::uint8_t length = 0;
  const std::uint8_t* onion = FindCdcFunctional(descriptor, kUnionFunctionalSubtype, length);
  if (onion == nullptr || length < 5) {
    return false;
  }
  out_slave = onion[4];
  return true;
}

/// Whether a given interface number exists in a configuration and its signature is an RNDIS data interface.
[[nodiscard]] bool HasDataInterface(const ::libusb_config_descriptor& config,
                                    std::uint8_t interface_number) {
  for (std::uint8_t i = 0; i < config.bNumInterfaces; ++i) {
    const ::libusb_interface& interface = config.interface[i];
    for (int alt = 0; alt < interface.num_altsetting; ++alt) {
      const ::libusb_interface_descriptor& descriptor = interface.altsetting[alt];
      if (descriptor.bInterfaceNumber != interface_number) {
        continue;
      }
      if (SignatureOf(descriptor) == rndis::kDataSignature) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

std::string DeviceCandidate::Describe() const {
  return std::format("Bus {:03d} Device {:03d}: {:04x}:{:04x}", bus_number, device_address,
                     vendor_id, product_id);
}

// =============================================================================
// Device discovery
// =============================================================================

Result<std::vector<DeviceCandidate>> FindRndisDevices(const Context& context,
                                                      const DeviceFilter& filter) {
  ::libusb_device** raw_list = nullptr;
  const ssize_t count = ::libusb_get_device_list(context.Raw(), &raw_list);
  if (count < 0) {
    return std::unexpected(
        Error::FromLibUsb(static_cast<int>(count), Tr(Msg::kUsbGetDeviceListFailed)));
  }
  // RAII releases the device list (unref_devices = 1).
  const std::unique_ptr<::libusb_device*, void (*)(::libusb_device**)> list_guard(
      raw_list, [](::libusb_device** list) { ::libusb_free_device_list(list, 1); });

  std::vector<DeviceCandidate> candidates;

  for (ssize_t index = 0; index < count; ++index) {
    ::libusb_device* device = raw_list[index];

    ::libusb_device_descriptor device_descriptor{};
    if (::libusb_get_device_descriptor(device, &device_descriptor) != LIBUSB_SUCCESS) {
      continue;
    }

    if (filter.vendor_id != 0 && device_descriptor.idVendor != filter.vendor_id) {
      continue;
    }
    if (filter.product_id != 0 && device_descriptor.idProduct != filter.product_id) {
      continue;
    }
    const std::uint8_t bus = ::libusb_get_bus_number(device);
    const std::uint8_t address = ::libusb_get_device_address(device);
    if (filter.bus_number != 0 && (bus != filter.bus_number || address != filter.device_address)) {
      continue;
    }

    // Look for the RNDIS communications interface configuration by configuration. Most devices have only one configuration, but there are exceptions.
    for (std::uint8_t config_index = 0; config_index < device_descriptor.bNumConfigurations;
         ++config_index) {
      ::libusb_config_descriptor* config = nullptr;
      if (::libusb_get_config_descriptor(device, config_index, &config) != LIBUSB_SUCCESS) {
        continue;
      }
      const std::unique_ptr<::libusb_config_descriptor, void (*)(::libusb_config_descriptor*)>
          config_guard(config, &::libusb_free_config_descriptor);

      for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const ::libusb_interface& interface = config->interface[i];
        for (int alt = 0; alt < interface.num_altsetting; ++alt) {
          const ::libusb_interface_descriptor& descriptor = interface.altsetting[alt];
          const rndis::InterfaceSignature signature = SignatureOf(descriptor);

          if (!rndis::IsRndisControlSignature(signature)) {
            continue;
          }
          if (LooksLikeRealAcmModem(descriptor)) {
            TETHERKITNEXT_DEBUG_TR(Msg::kUsbSkippedAcmModem, device_descriptor.idVendor,
                               device_descriptor.idProduct, descriptor.bInterfaceNumber);
            continue;
          }

          DeviceCandidate candidate;
          candidate.vendor_id = device_descriptor.idVendor;
          candidate.product_id = device_descriptor.idProduct;
          candidate.bus_number = bus;
          candidate.device_address = address;
          candidate.control_interface = descriptor.bInterfaceNumber;
          candidate.signature = signature;

          // Find the paired data interface: prefer the CDC Union descriptor.
          std::uint8_t slave = 0;
          bool resolved = false;
          if (TryReadUnionSlaveInterface(descriptor, slave) &&
              HasDataInterface(*config, slave)) {
            candidate.data_interface = slave;
            resolved = true;
          }

          // Android quirk: on many Android devices the CDC Union points to a nonexistent interface number,
          // or there is no CDC functional descriptor at all. In this case Linux's android_rndis_quirk
          // hard-codes "interface 0 = control, interface 1 = data", and requires that the probed
          // communications interface really is 0. We do the same here.
          if (!resolved && candidate.control_interface == kAndroidQuirkControlInterface &&
              HasDataInterface(*config, kAndroidQuirkDataInterface)) {
            candidate.data_interface = kAndroidQuirkDataInterface;
            candidate.used_android_quirk = true;
            resolved = true;
          }

          if (!resolved) {
            TETHERKITNEXT_DEBUG_TR(Msg::kUsbNoPairedDataInterface, candidate.Describe(),
                               candidate.control_interface);
            continue;
          }

          // DEBUG rather than INFO: enumeration is called periodically (the GUI scans every 2 seconds),
          // and at INFO level this line would flood the log with a whole column of repeats. "What was found" is
          // left to the caller to decide how to present -- the CLI prints the list itself, and the GUI shows it in the device card.
          TETHERKITNEXT_DEBUG_TR(
              Msg::kUsbDeviceFound, candidate.Describe(), candidate.control_interface,
              candidate.data_interface, signature.interface_class, signature.interface_subclass,
              signature.interface_protocol,
              candidate.used_android_quirk ? Text(Msg::kUsbViaAndroidQuirk) : std::string_view{});
          candidates.push_back(candidate);
          // Only the first matching communications interface is taken for a device.
          goto next_device;
        }
      }
    }
  next_device:;
  }

  return candidates;
}

// =============================================================================
// Device
// =============================================================================

Result<std::unique_ptr<Device>> Device::Open(const Context& context,
                                             const DeviceCandidate& candidate) {
  ::libusb_device** raw_list = nullptr;
  const ssize_t count = ::libusb_get_device_list(context.Raw(), &raw_list);
  if (count < 0) {
    return std::unexpected(
        Error::FromLibUsb(static_cast<int>(count), Tr(Msg::kUsbGetDeviceListFailed)));
  }
  const std::unique_ptr<::libusb_device*, void (*)(::libusb_device**)> list_guard(
      raw_list, [](::libusb_device** list) { ::libusb_free_device_list(list, 1); });

  // Locate the device by bus + address (more precise than VID:PID, and can distinguish multiple devices of the same model).
  ::libusb_device* target = nullptr;
  for (ssize_t index = 0; index < count; ++index) {
    if (::libusb_get_bus_number(raw_list[index]) == candidate.bus_number &&
        ::libusb_get_device_address(raw_list[index]) == candidate.device_address) {
      target = raw_list[index];
      break;
    }
  }
  if (target == nullptr) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kUsbDeviceGone, candidate.Describe())));
  }

  auto device = std::unique_ptr<Device>(new Device());
  device->candidate_ = candidate;
  device->description_ = candidate.Describe();
  device->speed_ = ::libusb_get_device_speed(target);

  const int open_rc = ::libusb_open(target, &device->handle_);
  if (open_rc != LIBUSB_SUCCESS) {
    return std::unexpected(
        Error::FromLibUsb(open_rc, Tr(Msg::kUsbOpenFailed, device->description_)));
  }

  // **Deliberately not calling libusb_set_auto_detach_kernel_driver** -- see the header explanation:
  // on macOS it triggers a destructive re-enumeration of the whole device, while nobody occupies the RNDIS interface to begin with.

  // Claim the two interfaces. The communications interface comes first, because the control channel uses it.
  const auto claim = [&device](std::uint8_t interface_number, std::string_view role,
                               bool& claimed_flag) -> Status {
    const int rc = ::libusb_claim_interface(device->handle_, interface_number);
    if (rc == LIBUSB_SUCCESS) {
      claimed_flag = true;
      return Ok();
    }
    Error error = Error::FromLibUsb(rc, Tr(Msg::kUsbClaimFailed, role, interface_number));
    if (rc == LIBUSB_ERROR_ACCESS) {
      return std::unexpected(std::move(error).WithContext(Tr(Msg::kUsbClaimBusyHint)));
    }
    if (rc == LIBUSB_ERROR_NOT_FOUND) {
      return std::unexpected(std::move(error).WithContext(Tr(Msg::kUsbClaimNotFoundHint)));
    }
    return std::unexpected(std::move(error));
  };

  TETHERKITNEXT_RETURN_IF_ERROR(
      claim(candidate.control_interface, Text(Msg::kUsbControlInterface),
            device->control_interface_claimed_));
  TETHERKITNEXT_RETURN_IF_ERROR(
      claim(candidate.data_interface, Text(Msg::kUsbDataInterface),
            device->data_interface_claimed_));

  // Parse the endpoints.
  ::libusb_config_descriptor* config = nullptr;
  const int config_rc = ::libusb_get_active_config_descriptor(target, &config);
  if (config_rc != LIBUSB_SUCCESS) {
    return std::unexpected(Error::FromLibUsb(config_rc, Tr(Msg::kUsbReadActiveConfigFailed)));
  }
  const std::unique_ptr<::libusb_config_descriptor, void (*)(::libusb_config_descriptor*)>
      config_guard(config, &::libusb_free_config_descriptor);

  TETHERKITNEXT_RETURN_IF_ERROR(device->ResolveEndpoints(*config));

  TETHERKITNEXT_INFO_TR(Msg::kUsbClaimed, device->description_, device->SpeedName(),
                    device->bulk_in_endpoint_, device->bulk_out_endpoint_,
                    device->bulk_max_packet_size_,
                    device->interrupt_in_endpoint_ == 0
                        ? std::string{Text(Msg::kUsbNoInterruptEndpointShort)}
                        : std::format("0x{:02x}", device->interrupt_in_endpoint_));

  return device;
}

Device::~Device() {
  if (handle_ == nullptr) {
    return;
  }
  // Order: release interfaces -> close handle.
  //
  // WARNING: Precondition: all asynchronous transfers must already be fully reclaimed. libusb_close **will not** reclaim
  // in-flight transfers for you (it just nulls dev_handle and prints a log), and afterwards IOKit aborts will still make
  // callbacks run on libusb's internal thread -- if the transfer has been freed by then it is a UAF.
  // Guaranteeing this is TransferPool's responsibility; see its Shutdown().
  if (data_interface_claimed_) {
    ::libusb_release_interface(handle_, candidate_.data_interface);
  }
  if (control_interface_claimed_) {
    ::libusb_release_interface(handle_, candidate_.control_interface);
  }
  ::libusb_close(handle_);
  handle_ = nullptr;
  TETHERKITNEXT_DEBUG_TR(Msg::kUsbClosed, description_);
}

Status Device::ResolveEndpoints(const ::libusb_config_descriptor& config) {
  for (std::uint8_t i = 0; i < config.bNumInterfaces; ++i) {
    const ::libusb_interface& interface = config.interface[i];
    for (int alt = 0; alt < interface.num_altsetting; ++alt) {
      const ::libusb_interface_descriptor& descriptor = interface.altsetting[alt];
      const bool is_data = descriptor.bInterfaceNumber == candidate_.data_interface;
      const bool is_control = descriptor.bInterfaceNumber == candidate_.control_interface;
      if (!is_data && !is_control) {
        continue;
      }

      for (std::uint8_t e = 0; e < descriptor.bNumEndpoints; ++e) {
        const ::libusb_endpoint_descriptor& endpoint = descriptor.endpoint[e];
        const auto type = static_cast<std::uint8_t>(endpoint.bmAttributes &
                                                    LIBUSB_TRANSFER_TYPE_MASK);
        const bool is_in = (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                           LIBUSB_ENDPOINT_IN;

        if (is_data && type == LIBUSB_TRANSFER_TYPE_BULK) {
          if (is_in && bulk_in_endpoint_ == 0) {
            bulk_in_endpoint_ = endpoint.bEndpointAddress;
            bulk_max_packet_size_ = endpoint.wMaxPacketSize;
          } else if (!is_in && bulk_out_endpoint_ == 0) {
            bulk_out_endpoint_ = endpoint.bEndpointAddress;
            if (bulk_max_packet_size_ == 0) {
              bulk_max_packet_size_ = endpoint.wMaxPacketSize;
            }
          }
        } else if (is_control && type == LIBUSB_TRANSFER_TYPE_INTERRUPT && is_in &&
                   interrupt_in_endpoint_ == 0) {
          interrupt_in_endpoint_ = endpoint.bEndpointAddress;
          interrupt_max_packet_size_ = endpoint.wMaxPacketSize;
        }
      }
    }
  }

  if (bulk_in_endpoint_ == 0 || bulk_out_endpoint_ == 0) {
    return std::unexpected(Error::Generic(Tr(Msg::kUsbBulkEndpointsMissing,
                                             candidate_.data_interface, bulk_in_endpoint_,
                                             bulk_out_endpoint_)));
  }
  if (bulk_max_packet_size_ == 0) {
    return std::unexpected(Error::Generic(Tr(Msg::kUsbBulkMaxPacketSizeZero)));
  }
  // A missing interrupt endpoint is legal: Linux's host driver ignores it entirely and polls the control endpoint instead.
  return Ok();
}

std::string_view Device::SpeedName() const noexcept {
  switch (speed_) {
    case LIBUSB_SPEED_LOW:
      return "low-speed 1.5Mbps";
    case LIBUSB_SPEED_FULL:
      return "full-speed 12Mbps";
    case LIBUSB_SPEED_HIGH:
      return "high-speed 480Mbps";
    case LIBUSB_SPEED_SUPER:
      return "SuperSpeed 5Gbps";
    case LIBUSB_SPEED_SUPER_PLUS:
      return "SuperSpeed+ 10Gbps";
    default:
      return Text(Msg::kUsbUnknownSpeed);
  }
}

Status Device::ClearHalt(std::uint8_t endpoint) {
  const int rc = ::libusb_clear_halt(handle_, endpoint);
  if (rc != LIBUSB_SUCCESS) {
    return std::unexpected(
        Error::FromLibUsb(rc, Tr(Msg::kUsbClearHaltFailed, endpoint)));
  }
  return Ok();
}

// =============================================================================
// UsbControlChannel
// =============================================================================

UsbControlChannel::UsbControlChannel(Device& device, std::uint32_t timeout_millis)
    : device_(&device), timeout_millis_(timeout_millis) {
  response_buffer_.resize(rndis::kControlBufferBytes);
  // The buffer takes the endpoint's actual wMaxPacketSize (RNDIS notifications are 8 bytes, but do not hard-code this value).
  notification_buffer_.resize(
      std::max<std::size_t>(rndis::kNotificationBytes, device.InterruptMaxPacketSize()));
}

UsbControlChannel::~UsbControlChannel() {
  StopNotificationListener();
  if (notification_transfer_ != nullptr) {
    ::libusb_free_transfer(notification_transfer_);
    notification_transfer_ = nullptr;
  }
}

// =============================================================================
// Asynchronous interrupt notification listening
//
// See the explanation in device.h: a synchronous interrupt transfer blocks forever on macOS,
// because darwin uses ReadPipeAsync (the no-timeout variant), and the timeout parameter is not honored.
// =============================================================================

Status UsbControlChannel::StartNotificationListener() {
  if (device_->InterruptInEndpoint() == 0) {
    TETHERKITNEXT_DEBUG_TR(Msg::kUsbNoInterruptEndpoint);
    return Ok();
  }
  if (notification_transfer_ != nullptr) {
    return std::unexpected(Error::Generic(Tr(Msg::kUsbNotificationAlreadyRunning)));
  }

  notification_transfer_ = ::libusb_alloc_transfer(0);
  if (notification_transfer_ == nullptr) {
    return std::unexpected(Error::Generic(Tr(Msg::kUsbAllocInterruptTransferFailed)));
  }

  // Pass timeout 0: the interrupt endpoint is "only comes when something happens" anyway, and waiting forever is exactly the semantics we want.
  // It will not hang any thread here -- the completion callback runs on the libusb event thread.
  ::libusb_fill_interrupt_transfer(
      notification_transfer_, device_->Handle(), device_->InterruptInEndpoint(),
      reinterpret_cast<unsigned char*>(notification_buffer_.data()),
      static_cast<int>(notification_buffer_.size()),
      &UsbControlChannel::NotificationCallbackTrampoline, this, /*timeout=*/0);

  notification_in_flight_.store(true, std::memory_order_release);
  const int rc = ::libusb_submit_transfer(notification_transfer_);
  if (rc != LIBUSB_SUCCESS) {
    notification_in_flight_.store(false, std::memory_order_release);
    return std::unexpected(Error::FromLibUsb(rc, Tr(Msg::kUsbSubmitInterruptFailed)));
  }
  TETHERKITNEXT_DEBUG_TR(Msg::kUsbNotificationStarted, device_->InterruptInEndpoint());
  return Ok();
}

void UsbControlChannel::StopNotificationListener() {
  if (notification_transfer_ == nullptr) {
    return;
  }
  notification_stopping_.store(true, std::memory_order_release);

  if (notification_in_flight_.load(std::memory_order_acquire)) {
    const int rc = ::libusb_cancel_transfer(notification_transfer_);
    if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
      TETHERKITNEXT_DEBUG_TR(Msg::kUsbCancelInterruptReturned, ::libusb_error_name(rc));
    }
  }

  // Wait for the callback to come back. WARNING: same as for the data channel: **this function must not be called from the libusb event thread**,
  // otherwise it is waiting on itself. After a timeout the transfer is not freed (better to leak than to use-after-free).
  constexpr int kMaxWaitMillis = 2000;
  for (int waited = 0;
       notification_in_flight_.load(std::memory_order_acquire) && waited < kMaxWaitMillis;
       ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (notification_in_flight_.load(std::memory_order_acquire)) {
    TETHERKITNEXT_ERROR_TR(Msg::kUsbInterruptReclaimTimeout);
    notification_transfer_ = nullptr;  // deliberate leak
  }
}

void UsbControlChannel::NotificationCallbackTrampoline(::libusb_transfer* transfer) {
  static_cast<UsbControlChannel*>(transfer->user_data)->OnNotificationComplete();
}

void UsbControlChannel::OnNotificationComplete() noexcept {
  // This function runs on the libusb event thread and does only very light work.
  const ::libusb_transfer* transfer = notification_transfer_;
  bool resubmit = true;

  switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
      if (static_cast<std::uint32_t>(transfer->actual_length) >= rndis::kNotificationBytes) {
        const std::uint32_t notification =
            LoadLe32(notification_buffer_.data() + rndis::kNotificationValueOffset);
        if (notification == rndis::kNotificationResponseAvailable) {
          notification_pending_.store(true, std::memory_order_release);
        } else {
          TETHERKITNEXT_TRACE_TR(Msg::kUsbUnknownNotification, notification);
        }
      }
      break;

    case LIBUSB_TRANSFER_CANCELLED:
    case LIBUSB_TRANSFER_NO_DEVICE:
      resubmit = false;
      break;

    case LIBUSB_TRANSFER_STALL:
      // Interrupt endpoint STALL: clear it and continue. If it cannot be cleared, give up listening and degrade to polling the control endpoint
      // (Linux's host driver does not use the interrupt endpoint at all, so this is not fatal).
      if (const auto status = device_->ClearHalt(device_->InterruptInEndpoint()); !status) {
        TETHERKITNEXT_WARN_TR(Msg::kUsbInterruptHaltClearFailed, status.error().ToString());
        resubmit = false;
      }
      break;

    default:
      TETHERKITNEXT_TRACE_TR(Msg::kUsbInterruptTransferStatus,
                         static_cast<int>(transfer->status));
      break;
  }

  if (notification_stopping_.load(std::memory_order_acquire)) {
    resubmit = false;
  }
  if (resubmit && ::libusb_submit_transfer(notification_transfer_) == LIBUSB_SUCCESS) {
    return;  // still in flight
  }
  notification_in_flight_.store(false, std::memory_order_release);
}

Status UsbControlChannel::SendMessage(std::span<const std::byte> message) {
  if (message.empty() || message.size() > 0xFFFF) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kUsbControlMessageLengthInvalid, message.size())));
  }

  // SEND_ENCAPSULATED_COMMAND: bmRequestType=0x21 (OUT|Class|Interface),
  // bRequest=0x00, wValue=0, wIndex=communications-class interface number, wLength=message length.
  const int transferred = ::libusb_control_transfer(
      device_->Handle(), rndis::kControlOutRequestType, rndis::kRequestSendEncapsulatedCommand,
      /*wValue=*/0, device_->Candidate().control_interface,
      // libusb's C interface wants unsigned char*; the content is not modified here.
      const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(message.data())),
      static_cast<std::uint16_t>(message.size()), timeout_millis_);

  if (transferred < 0) {
    return std::unexpected(Error::FromLibUsb(transferred, Tr(Msg::kUsbSendEncapsulatedFailed)));
  }
  if (static_cast<std::size_t>(transferred) != message.size()) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kUsbSendEncapsulatedShort, transferred, message.size())));
  }
  return Ok();
}

Result<std::span<const std::byte>> UsbControlChannel::ReceiveMessage() {
  // Zero before every read: the device may write only part of it, and leftover old data would make the parser read phantom fields.
  std::memset(response_buffer_.data(), 0, response_buffer_.size());

  // GET_ENCAPSULATED_RESPONSE: bmRequestType=0xA1 (IN|Class|Interface),
  // bRequest=0x01, wValue=0, wIndex=communications-class interface number.
  const int transferred = ::libusb_control_transfer(
      device_->Handle(), rndis::kControlInRequestType, rndis::kRequestGetEncapsulatedResponse,
      /*wValue=*/0, device_->Candidate().control_interface,
      reinterpret_cast<unsigned char*>(response_buffer_.data()),
      static_cast<std::uint16_t>(response_buffer_.size()), timeout_millis_);

  if (transferred < 0) {
    return std::unexpected(
        Error::FromLibUsb(transferred, Tr(Msg::kUsbGetEncapsulatedFailed)));
  }

  // Spec: when the device has no valid response yet it returns **1 byte of 0x00**, rather than a STALL.
  // So any result shorter than an RNDIS message header (8 bytes) is always treated as "not ready yet",
  // and an empty view is returned for the caller to retry -- this is **not** an error.
  if (static_cast<std::uint32_t>(transferred) < rndis::kMessageHeaderBytes) {
    return std::span<const std::byte>{};
  }
  return std::span<const std::byte>{response_buffer_.data(),
                                    static_cast<std::size_t>(transferred)};
}

rndis::NotificationResult UsbControlChannel::WaitForNotification(std::uint32_t timeout_millis) {
  if (device_->InterruptInEndpoint() == 0 || notification_transfer_ == nullptr) {
    return rndis::NotificationResult::kNotSupported;
  }

  // Only check (and consume) the atomic flag set by the event thread. **Never enter libusb's wait path** --
  // that is exactly the cause of the control thread hanging (see the explanation in device.h).
  if (notification_pending_.exchange(false, std::memory_order_acq_rel)) {
    return rndis::NotificationResult::kResponseAvailable;
  }
  if (timeout_millis == 0) {
    return rndis::NotificationResult::kTimeout;
  }

  // When it needs to wait a while, poll this flag with our own small-step sleeps rather than letting libusb do the waiting.
  // This way the worst case is only waiting an extra 1 ms, and it never blocks forever.
  for (std::uint32_t waited = 0; waited < timeout_millis; ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (notification_pending_.exchange(false, std::memory_order_acq_rel)) {
      return rndis::NotificationResult::kResponseAvailable;
    }
  }
  return rndis::NotificationResult::kTimeout;
}

}  // namespace tetherkitnext::usb
