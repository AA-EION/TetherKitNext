// libusb 上下文与事件循环线程。
//
// ★ macOS 上 libusb 的线程模型（实测 + 源码确认，与直觉不同，务必读懂）★
//
//   libusb 的 darwin 后端自己起了一个内部线程（org.libusb.device-hotplug）跑
//   CFRunLoop，IOKit 的完成通知在那个线程上到达。但**用户的 transfer 回调不在
//   那个线程执行** —— darwin_async_io_callback 只是把 transfer 挂进
//   ctx->completed_transfers 并写 event pipe，真正调用回调的是**任何调用
//   libusb_handle_events*() 的线程**。
//
//   由此推出三条铁律：
//
//   1. 必须有人持续调用 libusb_handle_events*()，否则回调永远不会被调用。
//      本类就是干这个的：一个专用线程死循环 handle_events。
//
//   2. **回调里绝对不能调用同步 API**（libusb_control_transfer /
//      libusb_bulk_transfer）。它们开头就是
//      `if (usbi_handling_events(ctx)) return LIBUSB_ERROR_BUSY;`，
//      这是个 TLS 判断 —— 从事件线程（含任何回调内部）调用必然失败。
//      → RNDIS 控制通道因此必须跑在**独立线程**上，见 rndis/state_machine。
//
//   3. **回调里绝对不能做阻塞 I/O**。usbi_handle_transfer_completion 调用回调时
//      持有 ctx->event_waiters_lock；另一个线程正在等同步传输完成时要抢这把锁，
//      回调阻塞多久就把它拖多久。
//      → RX 回调只做「解 RNDIS 包 + memcpy 进无锁队列 + 立刻 resubmit」，
//        真正的 BPF write() 交给另一个线程。
//
//   4. 回调里**直接 resubmit 同一个 transfer 是官方支持的**
//      （usbi_handle_transfer_completion 在调回调前已经把它移出 flying list
//      并清了 IN_FLIGHT 标志，且不持 itransfer->lock）。这是维持 USB 管道满载
//      的关键手法。
#pragma once

#include <libusb.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::usb {

/// libusb 上下文 + 专用事件循环线程。
///
/// 生命周期约束：所有设备句柄与 transfer 都必须在本对象析构**之前**释放完毕。
/// 析构顺序 = 停事件线程 → libusb_exit。
class Context {
 public:
  /// 初始化 libusb 并启动事件线程。
  [[nodiscard]] static Result<std::unique_ptr<Context>> Create();

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(Context&&) = delete;

  ~Context();

  [[nodiscard]] ::libusb_context* Raw() const noexcept { return context_; }

  /// libusb 版本串，用于日志。
  [[nodiscard]] static std::string VersionString();

  /// 是否支持热插拔（darwin 上恒为 true）。
  [[nodiscard]] static bool SupportsHotplug() noexcept;

  /// 请求事件线程退出。可从任意线程调用，幂等。
  void RequestStop() noexcept;

  /// 事件线程是否仍在运行。
  [[nodiscard]] bool Running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

 private:
  Context() = default;

  /// 事件线程主体。
  void RunEventLoop() noexcept;

  ::libusb_context* context_ = nullptr;
  std::thread event_thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
};

}  // namespace tetherkitnext::usb
