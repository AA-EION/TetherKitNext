// Internal utilities of the C ABI implementation layer. **Not installed externally**; used only inside src/capi.
//
// This centralizes three repetitive chores of the "C++ world <-> C world" boundary: copying std::string_view into
// fixed-size buffers, translating tetherkitnext::Error into tk_error_t, and getting the wall-clock time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/error.h"

namespace tetherkitnext::capi {

/// Copies text into a fixed-size buffer and guarantees NUL termination, truncating when too long.
///
/// * Truncation must land on a UTF-8 character boundary *
///   This project's error messages can be Chinese, where one Chinese character is 3 bytes. Cutting hard by bytes would leave
///   half a character at the end of the buffer, and when Swift's `String(cString:)` meets an illegal sequence the **whole string becomes replacement characters**,
///   which amounts to destroying the entire error message. So after truncating, back up to the nearest character start.
inline void CopyText(char* destination, std::size_t capacity, std::string_view text) noexcept {
  if (destination == nullptr || capacity == 0) {
    return;
  }
  std::size_t length = text.size() < capacity - 1 ? text.size() : capacity - 1;

  // length < text.size() means truncation occurred: if the cut point falls in the middle of a multi-byte sequence
  // (text[length] is a 0b10xxxxxx continuation byte), back up all the way to the start of that sequence.
  while (length > 0 && length < text.size() &&
         (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U) {
    --length;
  }

  std::memcpy(destination, text.data(), length);
  destination[length] = '\0';
}

/// Array version, saving writing sizeof everywhere.
template <std::size_t N>
inline void CopyText(char (&destination)[N], std::string_view text) noexcept {
  CopyText(destination, N, text);
}

/// Translates a C++-side error into a C struct. `out_error` may be nullptr (the caller does not care about the cause).
inline void FillError(tk_error_t* out_error, const Error& error) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = static_cast<std::int32_t>(error.Domain());
  out_error->code = error.Code();
  // ToString() is used rather than Context(): the former includes the domain and code such as "[libusb: LIBUSB_ERROR_ACCESS(-3)]",
  // which users can paste straight to us when reporting errors.
  CopyText(out_error->message, error.ToString());
}

/// Fills a pure logic error without an underlying error code.
inline void FillGenericError(tk_error_t* out_error, std::string_view message) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = TK_ERROR_DOMAIN_GENERIC;
  out_error->code = 0;
  CopyText(out_error->message, message);
}

/// Clears the error struct to "no error". Called on success paths, so callers do not read leftovers from the last time.
inline void ClearError(tk_error_t* out_error) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = TK_ERROR_DOMAIN_GENERIC;
  out_error->code = 0;
  out_error->message[0] = '\0';
}

/// The quadruple that uniquely identifies "one device plugged into the bus".
///
/// Bus addresses may be reused by the system after unplug/replug, so VID:PID is included as well to avoid attaching one device's memory
/// to another device that takes over the same address.
struct DeviceIdentity {
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;

  [[nodiscard]] bool operator==(const DeviceIdentity&) const noexcept = default;
};

/// Takes the device identity from an enumeration result.
[[nodiscard]] inline DeviceIdentity IdentityOf(const tk_device_info_t& info) noexcept {
  return DeviceIdentity{.bus_number = info.bus_number,
                        .device_address = info.device_address,
                        .vendor_id = info.vendor_id,
                        .product_id = info.product_id};
}

/// The string descriptors of a device most recently **read successfully**.
struct RememberedDeviceStrings {
  DeviceIdentity identity;
  char manufacturer[TK_USB_STRING_CAPACITY]{};
  char product[TK_USB_STRING_CAPACITY]{};
  char serial[TK_USB_STRING_CAPACITY]{};
};

/// Maintains the memory of string descriptors: remember when read, backfill from memory when not read.
///
/// * Why it is needed *
///   String descriptors need libusb_open to read, and during a session the device is held exclusively by this process,
///   so the read is skipped by the caller or fails outright -- but the device itself has not changed, and "could not get it this time" does not mean
///   "the device has no name". Without backfilling, a single enumeration at the moment of connecting would overwrite
///   "vivo iQOO Z10x" on the UI with "USB device 2d95:600b" and leave it hanging there for the entire session.
///
/// Rules (`infos` are the devices of this enumeration whose base fields are already filled):
///   - If **any** of the three strings of `infos[i]` is non-empty -> it was read this time; record it into `memory` by overwriting;
///   - All empty -> not read (skipped or the device is occupied); if `memory` has a memory of the same identity, backfill it;
///   - Entries in `memory` whose identity is not in `present` (all devices currently on the bus) are cleared --
///     the device has been unplugged and the memory is stale; if another device whose name cannot be read is plugged in at the same address,
///     it is better to show VID:PID than to attach the previous device's name to it.
///
/// `present` may be longer than `infos`: when the caller's array capacity is insufficient only the first few devices were filled,
/// and the rest are still present, so their memory must not be deleted by mistake.
/// Pure logic, touching no libusb, with locking the caller's responsibility -- this is what makes offline unit testing possible.
void ReconcileDeviceStrings(std::vector<RememberedDeviceStrings>& memory,
                            std::span<const DeviceIdentity> present,
                            std::span<tk_device_info_t> infos);

/// Wall-clock time since the Unix epoch (nanoseconds).
///
/// Events and logs use wall time rather than monotonic time: they are displayed to humans and must line up with the system log
/// in Console.app. Rate computation has its own monotonic time; see tk_session_status.monotonic_nanos.
[[nodiscard]] std::int64_t WallNanos() noexcept;

/// Validates a NIC name: accepts only `feth<digits>`.
///
/// This is not fastidiousness -- the NIC name gets spliced into the argv of `ipconfig` / `route`. Although we pass an array via
/// posix_spawn rather than going through a shell (which already rules out injection), restricting interface names to the form
/// that we created ourselves also blocks more realistic accidents such as "mistakenly passing en0 in and
/// wiping out the user's Wi-Fi configuration".
[[nodiscard]] bool IsValidFethName(std::string_view name) noexcept;

/// Installs the on-disk registration of feth interfaces (see orphan_cleanup.cc). Idempotent.
///
/// Called by tk_session_create -- registration is needed only when a NIC is really going to be created, keeping the root-free
/// group of interfaces (version, enumeration, preflight) free of side effects.
void InstallInterfaceRegistry();

}  // namespace tetherkitnext::capi
