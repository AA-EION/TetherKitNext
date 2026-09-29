// Discovery, claiming and endpoint resolution of RNDIS USB devices.
//
// * Can an RNDIS interface be claimed on macOS? -- Yes, and no root is needed *
//
//   This is this project's most critical feasibility premise; the conclusion comes from checking, one by one, the IOKitPersonalities of all
//   CDC-family kexts under /System/Library/Extensions:
//
//     AppleUSBACMControl0 = {class 2, subclass 2, protocol 0}
//     AppleUSBACMControl1 = {class 2, subclass 2, protocol 1}
//     AppleUSBECMControl  = {class 2, subclass 6, protocol *}
//     AppleUSBNCMControl  = {class 2, subclass 13, protocol *}
//     AppleUSBWCMControl  = {class 2, subclass 8, protocol *}
//
//   The RNDIS communications-class interface is {class 0x02, subclass 0x02, protocol **0xFF**} --
//   the protocol is neither 0 nor 1, and **no personality matches**. The {0xE0, 0x01, 0x03} variant commonly used by Android does not even have
//   a personality for class 0xE0.
//   That is to say, **the macOS kernel has no RNDIS driver at all** (which is exactly why third-party
//   kexts like HoRNDIS exist, and exactly why this project exists).
//
//   The data-class interface {0x0A, 0x00, 0x00} is indeed probed by AppleUSBECMData0 / AppleUSBACMData0,
//   but their start() needs to find a paired Control driver on the same device
//   (via bMasterInterface of the CDC Union descriptor); in the RNDIS scenario the communications interface is not taken over by any
//   Control driver, so the pairing lookup fails -> start() returns false -> the driver detaches,
//   and that interface's IORegistry node ends up with no children, so libusb's
//   darwin_kernel_driver_active() returns 0.
//
//   At the device level AppleUSBCDCCompositeDevice attaches (its IOProviderClass is
//   IOUSBHostDevice, with a very wide match surface), but it only does ConfigureDevice / publishes interfaces,
//   **does not open any interface**, and therefore does not affect USBInterfaceOpen.
//
//   -> Conclusion: an ordinary non-sandboxed command-line program **needs neither root nor an entitlement**;
//     libusb_open + libusb_claim_interface will succeed.
//     (This project as a whole still needs root, but that is a requirement of feth and BPF, not of libusb.)
//
// * Never enable libusb_set_auto_detach_kernel_driver *
//
//   On macOS it makes claim go through darwin_capture_claim_interface, and once
//   darwin_kernel_driver_active is judged true it goes to "detach" -- while darwin's detach
//   implementation **re-enumerates the entire device** (USBDeviceReEnumerate + CaptureDeviceMask),
//   and needs the com.apple.vm.device-access entitlement or root, otherwise it returns
//   LIBUSB_ERROR_ACCESS. In the RNDIS scenario nobody occupies the interface to begin with, so enabling it purely introduces a destructive operation.
#pragma once

#include <libusb.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/rndis/protocol.h"
#include "tetherkitnext/usb/context.h"
#include "tetherkitnext/rndis/control_channel.h"

namespace tetherkitnext::usb {

/// A candidate device recognized as RNDIS.
struct DeviceCandidate {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;

  /// Communications-class interface (carries the control channel + interrupt notifications).
  std::uint8_t control_interface = 0;
  /// Data-class interface (carries bulk IN/OUT).
  std::uint8_t data_interface = 0;
  /// The matched interface signature, used in logs to explain "which RNDIS form it was recognized as".
  rndis::InterfaceSignature signature{};
  /// Whether the Android quirk fallback path was taken (see the comments of DeviceFinder).
  bool used_android_quirk = false;

  /// In the form "Bus 020 Device 003: 18d1:4ee4".
  [[nodiscard]] std::string Describe() const;
};

/// Device filter conditions. All 0 / empty means no restriction.
struct DeviceFilter {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  /// Match only the specified address on the specified bus (both must be given together).
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;
};

/// Enumerates the currently connected devices that look like RNDIS.
///
/// Recognition logic (the order is the priority):
///   1. Traverse all interfaces of all configurations, looking for interfaces whose signature hits kControlSignature*;
///   2. Exclude fake RNDIS: those with class == 0x02 and a non-zero CDC ACM bmCapabilities are real
///      cdc-acm modems, not RNDIS. **This check only applies to class 0x02** --
///      RNDIS functions of the wireless class (0xE0) repurpose bmCapabilities for their own use.
///   3. Find the paired data interface: prefer reading bSlaveInterface of the CDC Union functional descriptor;
///   4. **Android quirk**: on many Android devices the CDC Union descriptor points to a nonexistent
///      interface number, or the CDC functional descriptors are missing entirely. In that case fall back to the hardcoded assumption "communications interface = 0,
///      data interface = 1" (which is what Linux's android_rndis_quirk does),
///      and require that the communications interface really is 0.
[[nodiscard]] Result<std::vector<DeviceCandidate>> FindRndisDevices(const Context& context,
                                                                   const DeviceFilter& filter = {});

/// An RNDIS device that has been opened with its interfaces claimed.
///
/// RAII: on destruction it is torn down in the order "release interfaces -> close handle".
/// **The caller must guarantee that all asynchronous transfers have been fully reclaimed before this object is destroyed** --
/// libusb_close will not reclaim in-flight transfers for you (it only nulls transfer->dev_handle
/// and prints a usbi_err), after which IOKit aborts will still make callbacks run on libusb's internal thread,
/// and if the transfer has been freed by then it is a use-after-free. See the teardown algorithm of TransferPool.
class Device {
 public:
  /// Opens the device and claims the two interfaces.
  [[nodiscard]] static Result<std::unique_ptr<Device>> Open(const Context& context,
                                                            const DeviceCandidate& candidate);

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  ~Device();

  [[nodiscard]] ::libusb_device_handle* Handle() const noexcept { return handle_; }

  [[nodiscard]] const DeviceCandidate& Candidate() const noexcept { return candidate_; }

  [[nodiscard]] std::string_view Describe() const noexcept { return description_; }

  // ---------------------------------------------------------------------------
  // Endpoints
  // ---------------------------------------------------------------------------

  /// Address of the bulk IN endpoint (device -> host).
  [[nodiscard]] std::uint8_t BulkInEndpoint() const noexcept { return bulk_in_endpoint_; }

  /// Address of the bulk OUT endpoint (host -> device).
  [[nodiscard]] std::uint8_t BulkOutEndpoint() const noexcept { return bulk_out_endpoint_; }

  /// Address of the interrupt IN endpoint; 0 means the device has no interrupt endpoint (legal; must fall back to polling).
  [[nodiscard]] std::uint8_t InterruptInEndpoint() const noexcept {
    return interrupt_in_endpoint_;
  }

  /// wMaxPacketSize of the bulk endpoints.
  ///
  /// Used in two places: (1) deriving the MaxTransferSize to claim in INITIALIZE_MSG;
  /// (2) judging whether the bulk OUT transfer length is exactly an integer multiple of it, to decide whether to pad 1 byte to avoid a ZLP.
  [[nodiscard]] std::uint16_t BulkMaxPacketSize() const noexcept { return bulk_max_packet_size_; }

  /// wMaxPacketSize of the interrupt endpoint (RNDIS notifications are fixed at 8 bytes).
  [[nodiscard]] std::uint16_t InterruptMaxPacketSize() const noexcept {
    return interrupt_max_packet_size_;
  }

  /// The device's USB speed, used for logs and throughput expectations.
  [[nodiscard]] int Speed() const noexcept { return speed_; }

  [[nodiscard]] std::string_view SpeedName() const noexcept;

  /// Clears the halt state of an endpoint.
  ///
  /// STALL recovery can be called directly inside a transfer callback: libusb_clear_halt goes through
  /// darwin_clear_halt -> ClearPipeStallBothEnds, which is one synchronous IOKit call,
  /// bypassing the event loop, with no usbi_handling_events guard. The cost is that it blocks the event thread
  /// for tens of microseconds to milliseconds, so call it only on a real STALL.
  [[nodiscard]] Status ClearHalt(std::uint8_t endpoint);

 private:
  Device() = default;

  /// Parses the three endpoints out of the interface descriptor.
  [[nodiscard]] Status ResolveEndpoints(const ::libusb_config_descriptor& config);

  ::libusb_device_handle* handle_ = nullptr;
  DeviceCandidate candidate_{};
  std::string description_;

  bool control_interface_claimed_ = false;
  bool data_interface_claimed_ = false;

  std::uint8_t bulk_in_endpoint_ = 0;
  std::uint8_t bulk_out_endpoint_ = 0;
  std::uint8_t interrupt_in_endpoint_ = 0;
  std::uint16_t bulk_max_packet_size_ = 0;
  std::uint16_t interrupt_max_packet_size_ = 0;
  int speed_ = 0;
};

/// Control channel implementation based on libusb.
///
/// **Must be used from a dedicated thread that is not a libusb event thread** -- the synchronous API
/// (libusb_control_transfer) returns LIBUSB_ERROR_BUSY on the event thread
/// (usbi_handling_events is a TLS check).
///
/// * Interrupt notifications go through an **asynchronous** transfer; this is not an optimization but a necessity *
///
///   If WaitForNotification were changed to use the synchronous libusb_interrupt_transfer,
///   **the control thread would hang forever**, with the stack:
///       Poll → WaitForNotification → do_sync_bulk_transfer
///            → sync_transfer_wait_for_completion → handle_events → poll(∞)
///
///   Causal chain:
///     1. The darwin backend marks all transfers with USBI_TRANSFER_OS_HANDLES_TIMEOUT,
///        meaning "timeouts are handled by IOKit; libusb does not time them itself"; so
///        libusb_get_next_timeout skips them and returns "no timeout",
///        and the poll() in sync_transfer_wait_for_completion blocks indefinitely;
///     2. And on the IOKit side, darwin's submit_interrupt_transfer uses
///        **ReadPipeAsync (the no-timeout variant)**, not the ReadPipeAsyncTO used by bulk --
///        **the timeout parameter of interrupt transfers is not honored at all**;
///     3. So when there is no data on the endpoint, this synchronous transfer never completes.
///
///   Changing timeout from 0 to 1 ms is **useless** (point 2 decides that it has no effect on interrupt endpoints).
///   The only correct solution is:
///   submit a resident **asynchronous** interrupt transfer and let it complete on the libusb event thread,
///   with WaitForNotification only checking an atomic flag, never entering libusb's wait path.
class UsbControlChannel final : public rndis::ControlChannel {
 public:
  UsbControlChannel(Device& device, std::uint32_t timeout_millis);

  UsbControlChannel(const UsbControlChannel&) = delete;
  UsbControlChannel& operator=(const UsbControlChannel&) = delete;
  UsbControlChannel(UsbControlChannel&&) = delete;
  UsbControlChannel& operator=(UsbControlChannel&&) = delete;

  ~UsbControlChannel() override;

  /// Submits the resident asynchronous interrupt transfer. A no-op when the device has no interrupt endpoint.
  [[nodiscard]] Status StartNotificationListener();

  /// Cancels the interrupt transfer and waits for the callback to finish reclaiming. **Must not be called from the libusb event thread.**
  void StopNotificationListener();

  [[nodiscard]] Status SendMessage(std::span<const std::byte> message) override;
  [[nodiscard]] Result<std::span<const std::byte>> ReceiveMessage() override;
  [[nodiscard]] rndis::NotificationResult WaitForNotification(std::uint32_t timeout_millis) override;

  [[nodiscard]] std::uint32_t TimeoutMillis() const noexcept override { return timeout_millis_; }

  [[nodiscard]] std::string_view Describe() const noexcept override {
    return device_->Describe();
  }

 private:
  static void NotificationCallbackTrampoline(::libusb_transfer* transfer);
  void OnNotificationComplete() noexcept;

  Device* device_;
  std::uint32_t timeout_millis_;

  /// Receive buffer of GET_ENCAPSULATED_RESPONSE.
  std::vector<std::byte> response_buffer_;
  /// Notification buffer of the interrupt IN (8 bytes).
  std::vector<std::byte> notification_buffer_;

  /// The resident asynchronous interrupt transfer.
  ::libusb_transfer* notification_transfer_ = nullptr;
  /// Whether the device has signaled that "a response can be fetched". Set by the event thread, consumed by the control thread.
  std::atomic<bool> notification_pending_{false};
  /// Whether an interrupt transfer is in flight. Used to wait for the callback to finish reclaiming at shutdown (avoiding use-after-free).
  std::atomic<bool> notification_in_flight_{false};
  std::atomic<bool> notification_stopping_{false};
};

}  // namespace tetherkitnext::usb
