// Three root-free things: version, environment preflight, device enumeration.
//
// The GUI itself (uid 501) calls only this group; the root-requiring session and NIC configuration are all handed to
// tetherkitnext-helper. This split is one of the core conclusions of docs/GUI-SPIKE.md.
#include <libusb.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "capi_support.h"
#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/net/feth_device.h"
#include "tetherkitnext/usb/context.h"
#include "tetherkitnext/usb/device.h"
#include "tetherkitnext/version.h"

namespace {

using tetherkitnext::capi::ClearError;
using tetherkitnext::capi::CopyText;
using tetherkitnext::capi::FillError;

/// Reads a USB string descriptor into a fixed-size buffer. An index of 0 means the device does not provide that string.
///
/// Uses libusb's `_ascii` variant: it replaces code points beyond ASCII in the UTF-16LE with '?'.
/// That is enough for us -- these strings are only used to let users tell two devices of the same model apart, and the vast majority of vendor names and
/// serial numbers are ASCII anyway. Doing a full UTF-16 -> UTF-8 conversion ourselves gains too little.
void ReadStringDescriptor(::libusb_device_handle* handle, std::uint8_t index, char* destination,
                          std::size_t capacity) noexcept {
  if (index == 0) {
    return;
  }
  std::array<unsigned char, TK_USB_STRING_CAPACITY> buffer{};
  const int length = ::libusb_get_string_descriptor_ascii(
      handle, index, buffer.data(), static_cast<int>(buffer.size()));
  if (length <= 0) {
    return;
  }
  CopyText(destination, capacity,
           std::string_view{reinterpret_cast<const char*>(buffer.data()),
                            static_cast<std::size_t>(length)});
}

/// Fills in the vendor name / product name / serial number on a best-effort basis.
///
/// Why "best effort": reading string descriptors requires libusb_open first, and the device may already be held exclusively by another process
/// on this machine (such as the running tetherkitnext-helper), in which case the darwin backend returns
/// LIBUSB_ERROR_ACCESS. That is not an error, just the name being unobtainable -- then the string memory backfills
/// the value read last time (see the end of tk_list_devices), and only with no memory either does it fall back to VID:PID.
/// It must never make the whole enumeration fail because of this.
void TryReadStrings(::libusb_context* context, tk_device_info_t& info) noexcept {
  ::libusb_device** list = nullptr;
  const ssize_t count = ::libusb_get_device_list(context, &list);
  if (count < 0) {
    return;
  }

  for (ssize_t i = 0; i < count; ++i) {
    if (::libusb_get_bus_number(list[i]) != info.bus_number ||
        ::libusb_get_device_address(list[i]) != info.device_address) {
      continue;
    }

    ::libusb_device_descriptor descriptor{};
    if (::libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS) {
      break;
    }
    ::libusb_device_handle* handle = nullptr;
    if (::libusb_open(list[i], &handle) != LIBUSB_SUCCESS) {
      break;
    }
    ReadStringDescriptor(handle, descriptor.iManufacturer, info.manufacturer,
                         sizeof(info.manufacturer));
    ReadStringDescriptor(handle, descriptor.iProduct, info.product, sizeof(info.product));
    ReadStringDescriptor(handle, descriptor.iSerialNumber, info.serial, sizeof(info.serial));
    ::libusb_close(handle);
    break;
  }

  // The second argument is 1: release together with the reference count of every device in the list.
  ::libusb_free_device_list(list, 1);
}

/// The process-wide shared libusb context, **for enumeration only**.
///
/// * Why it must be shared *
///   Creating a new context for every enumeration amounts to going through "initialize libusb -> start the event thread
///   -> start the IOKit runloop thread -> tear everything down after use" every single time. The GUI refreshes the device
///   list every few hundred milliseconds, so this overhead would repeat at the same frequency, and the log would also be flooded with "libusb initialized".
///
///   After sharing, the whole process initializes only once. A long-lived context still sees newly plugged-in devices --
///   libusb's darwin backend has its own hotplug thread maintaining the device list, which is the ordinary mechanism that all
///   libusb programs depend on.
///
/// A session (core::Runtime) has its own separate context and does not reuse this one: its lifetime is managed by the session
/// itself, and mixing them in would only complicate the shutdown order. libusb supports multiple contexts in one process.
///
/// On initialization failure returns nullptr and fills the error, and **does not cache the failure**; the next call retries.
tetherkitnext::usb::Context* SharedEnumerationContext(tk_error_t* out_error) {
  static std::mutex mutex;
  static std::unique_ptr<tetherkitnext::usb::Context> context;

  const std::lock_guard<std::mutex> guard(mutex);
  if (context == nullptr) {
    auto created = tetherkitnext::usb::Context::Create();
    if (!created) {
      FillError(out_error, created.error());
      return nullptr;
    }
    context = std::move(*created);
  }
  return context.get();
}

/// The process-wide memory of string descriptors (lock + entries), a function-local static like the shared enumeration context.
///
/// It is placed at process level rather than in the caller: the helper is a resident process, so after a mid-way GUI restart the first enumeration
/// can already get the backfilled names; for the pure logic see the explanation of ReconcileDeviceStrings.
struct StringMemory {
  std::mutex mutex;
  std::vector<tetherkitnext::capi::RememberedDeviceStrings> entries;
};

StringMemory& SharedStringMemory() {
  static StringMemory memory;
  return memory;
}

void FillDeviceInfo(const tetherkitnext::usb::DeviceCandidate& candidate,
                    tk_device_info_t& info) noexcept {
  info = tk_device_info_t{};
  info.vendor_id = candidate.vendor_id;
  info.product_id = candidate.product_id;
  info.bus_number = candidate.bus_number;
  info.device_address = candidate.device_address;
  info.control_interface = candidate.control_interface;
  info.data_interface = candidate.data_interface;
  info.interface_class = candidate.signature.interface_class;
  info.interface_subclass = candidate.signature.interface_subclass;
  info.interface_protocol = candidate.signature.interface_protocol;
  info.used_android_quirk = candidate.used_android_quirk;
  CopyText(info.description, candidate.Describe());
}

}  // namespace

namespace {

/// tk_language_t -> the C++ side's Language. Returns nullopt when out of range, so the caller ignores this setting.
[[nodiscard]] std::optional<tetherkitnext::Language> ToLanguage(std::int32_t value) noexcept {
  switch (value) {
    case TK_LANGUAGE_ENGLISH:
      return tetherkitnext::Language::kEnglish;
    case TK_LANGUAGE_CHINESE:
      return tetherkitnext::Language::kChinese;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::int32_t FromLanguage(tetherkitnext::Language language) noexcept {
  return language == tetherkitnext::Language::kChinese ? TK_LANGUAGE_CHINESE : TK_LANGUAGE_ENGLISH;
}

}  // namespace

void tk_set_language(int32_t language) {
  if (const std::optional<tetherkitnext::Language> parsed = ToLanguage(language);
      parsed.has_value()) {
    tetherkitnext::SetLanguage(*parsed);
  }
}

int32_t tk_get_language(void) {
  return FromLanguage(tetherkitnext::GetLanguage());
}

int32_t tk_detect_system_language(void) {
  return FromLanguage(tetherkitnext::DetectLanguageFromEnvironment());
}

void tk_version(tk_version_info_t* out_version) {
  if (out_version == nullptr) {
    return;
  }
  *out_version = tk_version_info_t{};
  const tetherkitnext::Version version = tetherkitnext::GetVersion();
  out_version->major = version.major;
  out_version->minor = version.minor;
  out_version->patch = version.patch;
  CopyText(out_version->text, tetherkitnext::GetVersionString());
  CopyText(out_version->build, tetherkitnext::GetBuildDescription());
  CopyText(out_version->libusb, tetherkitnext::usb::Context::VersionString());
}

tk_result_t tk_check_environment(tk_environment_t* out_environment) {
  if (out_environment == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }
  *out_environment = tk_environment_t{};

  out_environment->is_root = tetherkitnext::net::IsRunningAsRoot();

  // These sysctls are **creation-time snapshots** of feth; changing them after creation has no effect, so they must be checked in advance.
  // When unacceptable, hand the reason as-is to the GUI to display -- only when the user sees exactly which switch is turned on
  // do they know what to change.
  if (const auto status = tetherkitnext::net::VerifyFethSysctls(); status) {
    out_environment->sysctls_ok = true;
  } else {
    out_environment->sysctls_ok = false;
    CopyText(out_environment->sysctl_detail, status.error().ToString());
  }

  if (const auto max_mtu = tetherkitnext::net::QueryFethMaxMtu(); max_mtu) {
    out_environment->feth_max_mtu = *max_mtu;
  }

  // Deliberately always return success: "environment unacceptable" is a **result** to be shown to the user, not a call failure.
  return TK_OK;
}

tk_result_t tk_list_devices(tk_device_info_t* out_devices, size_t capacity, size_t* out_count,
                            bool read_strings, tk_error_t* out_error) {
  if (out_count == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }
  ClearError(out_error);
  *out_count = 0;

  tetherkitnext::usb::Context* context = SharedEnumerationContext(out_error);
  if (context == nullptr) {
    return TK_ERR_FAILED;
  }

  auto candidates = tetherkitnext::usb::FindRndisDevices(*context, {});
  if (!candidates) {
    FillError(out_error, candidates.error());
    return TK_ERR_FAILED;
  }

  *out_count = candidates->size();
  if (out_devices == nullptr) {
    return TK_OK;
  }

  const std::size_t writable = std::min(capacity, candidates->size());
  for (std::size_t i = 0; i < writable; ++i) {
    FillDeviceInfo((*candidates)[i], out_devices[i]);
    if (read_strings) {
      TryReadStrings(context->Raw(), out_devices[i]);
    }
  }

  // Remember when read, backfill when not read -- only then does the name not disappear from the UI during a session (read_strings=false or the device occupied).
  // present uses the complete candidate set rather than the writable prefix: when capacity is insufficient the devices further back
  // are still present, and their memory must not be cleared as "unplugged".
  {
    std::vector<tetherkitnext::capi::DeviceIdentity> present;
    present.reserve(candidates->size());
    for (const tetherkitnext::usb::DeviceCandidate& candidate : *candidates) {
      present.push_back({.bus_number = candidate.bus_number,
                         .device_address = candidate.device_address,
                         .vendor_id = candidate.vendor_id,
                         .product_id = candidate.product_id});
    }
    StringMemory& memory = SharedStringMemory();
    const std::lock_guard<std::mutex> guard(memory.mutex);
    tetherkitnext::capi::ReconcileDeviceStrings(memory.entries, present, {out_devices, writable});
  }
  return TK_OK;
}
