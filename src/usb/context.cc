#include "tetherkitnext/usb/context.h"

#include <format>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/common/scheduling.h"

namespace tetherkitnext::usb {
namespace {

/// 事件循环单次 handle_events 的阻塞上限。
///
/// 为什么可以用这么长的超时：darwin 后端给所有 bulk transfer 打了
/// USBI_TRANSFER_OS_HANDLES_TIMEOUT（超时由 IOKit 负责），而
/// libusb_get_next_timeout 会跳过带该标志的 transfer —— 所以只要在飞的都是
/// bulk，它恒返回「无超时」，事件线程完全靠 event pipe 唤醒即可。
/// 这里给 250 ms 只是为了让 RequestStop() 能在一个周期内被看到。
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
    // libusb_interrupt_event_handler 会往 event pipe 写一个字节，让阻塞在
    // handle_events 里的线程立刻返回。没有它就得等满一个超时周期。
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
    // 用带超时的版本而非 libusb_handle_events()：后者内部超时固定 60 秒，
    // 停机响应太慢；也不用 libusb_handle_events_completed()，因为我们的退出
    // 条件是自己的原子标志而非某个 transfer 的完成。
    const int rc = ::libusb_handle_events_timeout_completed(context_, &timeout, nullptr);

    if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_INTERRUPTED) {
      continue;
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      // 设备拔了。不是事件循环的错，交给上层的重连逻辑处理，这里继续跑。
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
