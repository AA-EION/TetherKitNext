// libusb context and the event loop thread.
//
// * libusb's threading model on macOS (measured + confirmed against source; differs from intuition, be sure to understand it) *
//
//   libusb's darwin backend itself starts an internal thread (org.libusb.device-hotplug) that runs
//   the CFRunLoop, and IOKit completion notifications arrive on that thread. But **the user's transfer callback does not execute on
//   that thread** -- darwin_async_io_callback only hangs the transfer onto
//   ctx->completed_transfers and writes the event pipe; what actually invokes the callback is **whichever thread
//   calls libusb_handle_events*()**.
//
//   From this follow three iron rules:
//
//   1. Someone must keep calling libusb_handle_events*(), otherwise callbacks are never invoked.
//      This class does exactly that: a dedicated thread loops on handle_events forever.
//
//   2. **A callback must absolutely not call a synchronous API** (libusb_control_transfer /
//      libusb_bulk_transfer). They start with
//      `if (usbi_handling_events(ctx)) return LIBUSB_ERROR_BUSY;`,
//      which is a TLS check -- calling from the event thread (including inside any callback) is bound to fail.
//      -> The RNDIS control channel therefore must run on a **separate thread**; see rndis/state_machine.
//
//   3. **A callback must absolutely not do blocking I/O.** When usbi_handle_transfer_completion invokes the callback it
//      holds ctx->event_waiters_lock; another thread waiting for a synchronous transfer to complete needs to grab this lock,
//      and however long the callback blocks, it holds that thread up just as long.
//      -> The RX callback only does "unpack RNDIS packets + memcpy into the lock-free queue + resubmit immediately",
//        and the real BPF write() is handed to another thread.
//
//   4. **Directly resubmitting the same transfer inside a callback is officially supported**
//      (usbi_handle_transfer_completion has already removed it from the flying list before calling the callback,
//      and cleared the IN_FLIGHT flag, and does not hold itransfer->lock). This is the key technique for keeping
//      the USB pipe saturated.
#pragma once

#include <libusb.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::usb {

/// libusb context + dedicated event loop thread.
///
/// Lifetime constraint: all device handles and transfers must be released completely **before** this object is destroyed.
/// Destruction order = stop the event thread -> libusb_exit.
class Context {
 public:
  /// Initializes libusb and starts the event thread.
  [[nodiscard]] static Result<std::unique_ptr<Context>> Create();

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(Context&&) = delete;

  ~Context();

  [[nodiscard]] ::libusb_context* Raw() const noexcept { return context_; }

  /// libusb version string, for logging.
  [[nodiscard]] static std::string VersionString();

  /// Whether hotplug is supported (always true on darwin).
  [[nodiscard]] static bool SupportsHotplug() noexcept;

  /// Requests the event thread to exit. May be called from any thread; idempotent.
  void RequestStop() noexcept;

  /// Whether the event thread is still running.
  [[nodiscard]] bool Running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

 private:
  Context() = default;

  /// The event thread body.
  void RunEventLoop() noexcept;

  ::libusb_context* context_ = nullptr;
  std::thread event_thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
};

}  // namespace tetherkitnext::usb
