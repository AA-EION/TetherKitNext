#include "tetherkitnext/usb/context.h"

#include <format>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/common/scheduling.h"

namespace tetherkitnext::usb {
namespace {

/// Upper bound of blocking for a single handle_events of the event loop.
///
/// Why such a long timeout can be used: the darwin backend marks all bulk transfers with
/// USBI_TRANSFER_OS_HANDLES_TIMEOUT (timeouts are IOKit's responsibility), and
/// libusb_get_next_timeout skips transfers carrying that flag -- so as long as everything in flight is
/// bulk, it always returns "no timeout", and the event thread can be woken purely by the event pipe.
/// 250 ms is given here only so that RequestStop() can be noticed within one period.
constexpr long kEventLoopTimeoutSeconds = 0;
constexpr long kEventLoopTimeoutMicros = 250'000;

}  // namespace

Result<std::unique_ptr<Context>> Context::Create() {
  auto context = std::unique_ptr<Context>(new Context());

  const int rc = ::libusb_init(&context->context_);
  if (rc != LIBUSB_SUCCESS) {
    return std::unexpected(Error::FromLibUsb(rc, Tr(Msg::kUsbInitFailed)));
  }

  TETHERKITNEXT_INFO_TR(Msg::kUsbInitialized, VersionString(),
                    Text(SupportsHotplug() ? Msg::kUsbHotplugSupported
                                           : Msg::kUsbHotplugUnsupported));

  context->running_.store(true, std::memory_order_release);
  context->event_thread_ = std::thread([raw = context.get()] { raw->RunEventLoop(); });
  return context;
}

Context::~Context() {
  RequestStop();

  if (event_thread_.joinable()) {
    // libusb_interrupt_event_handler writes a byte to the event pipe, making the thread blocked in
    // handle_events return immediately. Without it we would have to wait out a full timeout period.
    if (context_ != nullptr) {
      ::libusb_interrupt_event_handler(context_);
    }
    event_thread_.join();
  }

  if (context_ != nullptr) {
    ::libusb_exit(context_);
    context_ = nullptr;
  }
  TETHERKITNEXT_DEBUG_TR(Msg::kUsbContextReleased);
}

std::string Context::VersionString() {
  const ::libusb_version* version = ::libusb_get_version();
  if (version == nullptr) {
    return std::string{Text(Msg::kUsbUnknownVersion)};
  }
  return std::format("{}.{}.{}.{}", version->major, version->minor, version->micro,
                     version->nano);
}

bool Context::SupportsHotplug() noexcept {
  return ::libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0;
}

void Context::RequestStop() noexcept {
  const bool already = stop_requested_.exchange(true, std::memory_order_acq_rel);
  if (!already && context_ != nullptr) {
    ::libusb_interrupt_event_handler(context_);
  }
}

void Context::RunEventLoop() noexcept {
  ConfigureCurrentThread("usb-event", ThreadRole::kDataPath);
  TETHERKITNEXT_DEBUG_TR(Msg::kUsbEventThreadStarted);

  ::timeval timeout{};
  timeout.tv_sec = kEventLoopTimeoutSeconds;
  timeout.tv_usec = kEventLoopTimeoutMicros;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // The version with a timeout is used rather than libusb_handle_events(): the latter's internal timeout is a fixed 60 seconds,
    // and shutdown would respond too slowly; libusb_handle_events_completed() is not used either, because our exit
    // condition is our own atomic flag rather than the completion of some transfer.
    const int rc = ::libusb_handle_events_timeout_completed(context_, &timeout, nullptr);

    if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_INTERRUPTED) {
      continue;
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      // The device was unplugged. Not the event loop's fault; leave it to the upper layer's reconnection logic, and keep running here.
      TETHERKITNEXT_DEBUG_TR(Msg::kUsbEventLoopNoDevice);
      continue;
    }
    TETHERKITNEXT_ERROR_TR(Msg::kUsbHandleEventsFailed, ::libusb_error_name(rc), rc);
    break;
  }

  running_.store(false, std::memory_order_release);
  TETHERKITNEXT_DEBUG_TR(Msg::kUsbEventThreadExited);
}

}  // namespace tetherkitnext::usb
