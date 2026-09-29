// Abstraction of the RNDIS control channel.
//
// Why this interface lives in the rndis layer rather than the usb layer: it describes the **RNDIS protocol's** control channel
// semantics (send a message, fetch a message, wait for a "response is available" notification), not USB semantics.
// The state machine (tk_rndis) depends on it, and the libusb implementation (tk_usb) provides it -- so the dependency direction is
// the correct rndis <- usb, and tk_rndis never depends backward on tk_usb.
//
// RNDIS control messages do not go over the bulk endpoints, but over two class requests on the USB control endpoint (EP0):
//   SEND_ENCAPSULATED_COMMAND  (bmRequestType=0x21, bRequest=0x00)
//   GET_ENCAPSULATED_RESPONSE  (bmRequestType=0xA1, bRequest=0x01)
// plus the interrupt IN endpoint on the communications-class interface, used to notify "a response can be fetched".
//
// Why abstract into an interface: the state machine is the most logically complex part of the whole RNDIS implementation and the part that most needs testing,
// yet the development machine has no USB device at all. After abstracting the control channel away, the state machine can be driven entirely in memory,
// including paths that are extremely hard to reproduce on real hardware (the device proactively sending KEEPALIVE,
// INDICATE_STATUS cutting in, replay required after RESET, out-of-order responses, timeout retries).
//
// Using virtual functions here is completely fine performance-wise: the control channel has one keepalive round trip only every 5 seconds,
// and libusb control transfers on Apple Silicon are themselves at the **millisecond** level
// (libusb issue #1288: on M2 a control transfer is about 10x slower than on x64).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::rndis {

/// The timeout value to pass when you "only want a quick probe, not a real wait".
///
/// WARNING: **Do not pass 0.** On darwin libusb passes timeout to IOKit as both noDataTimeout and
/// completionTimeout, and **0 means wait forever** -- passing 0 would block the calling thread
/// forever in `libusb_wait_for_event`, hanging the entire control loop, unable even to respond to the shutdown signal.
/// (See items 12 and 13 of section 7 in AGENTS.md for details.)
/// libusb's synchronous API has no true non-blocking mode; the shortest wait achievable is 1 ms.
inline constexpr std::uint32_t kProbeOnlyTimeoutMillis = 1;

/// Result of waiting for a notification.
enum class NotificationResult : std::uint8_t {
  kResponseAvailable,  ///< The device explicitly notified that a response can be fetched.
  kTimeout,            ///< Wait timed out. **Not necessarily an error** -- see the note below.
  kNotSupported,       ///< The device has no interrupt endpoint; the caller should poll directly.
};

/// The RNDIS control channel.
///
/// Implementations must guarantee: all methods are called from the **same thread** (the state machine thread). This is not to
/// simplify the implementation, but a hard libusb constraint -- calling the synchronous API from the event thread returns
/// LIBUSB_ERROR_BUSY, so the control channel must exclusively own a non-event thread.
class ControlChannel {
 public:
  ControlChannel() = default;
  ControlChannel(const ControlChannel&) = delete;
  ControlChannel& operator=(const ControlChannel&) = delete;
  ControlChannel(ControlChannel&&) = delete;
  ControlChannel& operator=(ControlChannel&&) = delete;
  virtual ~ControlChannel() = default;

  /// Sends one RNDIS control message (SEND_ENCAPSULATED_COMMAND).
  [[nodiscard]] virtual Status SendMessage(std::span<const std::byte> message) = 0;

  /// Fetches one RNDIS control message (GET_ENCAPSULATED_RESPONSE).
  ///
  /// The returned view points into the implementation's internal buffer and is valid until the next call of this method.
  ///
  /// **Returning an empty view is not an error**: the spec says that when the device has no valid response yet it should return **1 byte of 0x00**
  /// rather than STALLing the control endpoint. So when the caller receives a result shorter than 8 bytes it must treat it as
  /// "not ready yet, retry later", not as a failure.
  [[nodiscard]] virtual Result<std::span<const std::byte>> ReceiveMessage() = 0;

  /// Waits for the device's RESPONSE_AVAILABLE notification.
  ///
  /// Handling of the interrupt endpoint must be compatible with both kinds of device behavior:
  ///   * The spec way: the host waits for an 8-byte notification on the interrupt IN
  ///     (two LE32: 0x00000001 = RESPONSE_AVAILABLE, 0);
  ///   * The Linux way: ignore the interrupt endpoint entirely and poll GET_ENCAPSULATED_RESPONSE directly
  ///     up to 10 times, 40 ms apart.
  /// Conversely, there are also **some devices that must have the interrupt endpoint read once before they answer on the control endpoint**.
  /// So the correct strategy is: wait for the notification first, and after a timeout still poll once. kTimeout is therefore not an error.
  ///
  /// @param timeout_millis Wait upper bound. **Passing 0 means wait forever** (consistent with libusb's
  ///        semantics) -- to "only probe", pass kProbeOnlyTimeoutMillis, not 0.
  ///        Implementations should defensively clamp 0; see UsbControlChannel.
  [[nodiscard]] virtual NotificationResult WaitForNotification(std::uint32_t timeout_millis) = 0;

  /// Timeout of control transfers (milliseconds).
  [[nodiscard]] virtual std::uint32_t TimeoutMillis() const noexcept = 0;

  /// Readable identifier for logging (such as "Bus 020 Device 003: 18d1:4ee4").
  [[nodiscard]] virtual std::string_view Describe() const noexcept = 0;
};

}  // namespace tetherkitnext::rndis
