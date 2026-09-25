// 内存版 RNDIS 数据通道，用于离线驱动桥接层。
//
// 它扮演「USB 设备」这一侧：
//   * StartReceiving 之后可以用 InjectFromDevice 注入「设备发来的帧」，由一个
//     内部生产者线程推进桥接层的 RX 队列 —— 这模拟 libusb 事件线程的角色；
//   * SendFrames 收下「主机要发给设备的帧」，供测试断言。
//
// 这样桥接层的线程模型、批处理、背压与统计就能在没有 USB 设备的机器上被完整
// 测试，包括 TSan 下的并发正确性。
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "tetherkitnext/common/frame_ring.h"
#include "tetherkitnext/usb/data_channel.h"

namespace tetherkitnext::testing {

/// 模拟设备行为的数据通道。
class MockDataChannel final : public usb::DataChannel {
 public:
  explicit MockDataChannel(std::uint32_t max_transfer_bytes = 16 * 1024)
      : max_transfer_bytes_(max_transfer_bytes) {}

  MockDataChannel(const MockDataChannel&) = delete;
  MockDataChannel& operator=(const MockDataChannel&) = delete;
  MockDataChannel(MockDataChannel&&) = delete;
  MockDataChannel& operator=(MockDataChannel&&) = delete;

  ~MockDataChannel() override { Shutdown(); }

  // ---------------------------------------------------------------------------
  // DataChannel 实现
  // ---------------------------------------------------------------------------

  [[nodiscard]] Status StartReceiving(FrameRing& rx_ring,
                                      DirectionCounters& rx_counters) override {
    rx_ring_ = &rx_ring;
    rx_counters_ = &rx_counters;
    receiving_.store(true, std::memory_order_release);
    return Ok();
  }

  [[nodiscard]] Result<usb::SendOutcome> SendFrames(std::span<const FrameView> frames) override {
    if (send_should_fail_.load(std::memory_order_acquire)) {
      return std::unexpected(Error::Generic("mock：按测试设置让发送失败"));
    }

    // 模拟传输池容量有限：一次最多吃下 accept_limit_ 帧，超出即背压。
    const std::uint32_t limit = accept_limit_.load(std::memory_order_acquire);
    const auto accepted =
        static_cast<std::uint32_t>(std::min<std::size_t>(frames.size(), limit));

    std::uint64_t bytes = 0;
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      for (std::size_t i = 0; i < accepted; ++i) {
        sent_to_device_.emplace_back(frames[i].data, frames[i].data + frames[i].length);
        sent_bytes_ += frames[i].length;
        bytes += frames[i].length;
      }
    }
    sent_frames_.fetch_add(accepted, std::memory_order_relaxed);
    return usb::SendOutcome{
        .consumed = accepted, .sent_frames = accepted, .sent_bytes = bytes, .skipped = 0};
  }

  [[nodiscard]] bool WaitForSendCapacity(std::uint32_t timeout_millis) override {
    wait_calls_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(wait_mutex_);
    return wait_cv_.wait_for(lock, std::chrono::milliseconds(timeout_millis), [this] {
      return shutdown_called_.load(std::memory_order_acquire) ||
             accept_limit_.load(std::memory_order_acquire) > 0;
    });
  }

  void Shutdown() override {
    receiving_.store(false, std::memory_order_release);
    shutdown_called_.store(true, std::memory_order_release);
    // 唤醒可能在 WaitForSendCapacity 里等容量的线程（与真实实现同款空临界区惯用法）。
    { const std::lock_guard<std::mutex> guard(wait_mutex_); }
    wait_cv_.notify_all();
  }

  [[nodiscard]] bool CanSend() const noexcept override {
    return accept_limit_.load(std::memory_order_acquire) > 0;
  }

  [[nodiscard]] std::uint32_t MaxTransferBytes() const noexcept override {
    return max_transfer_bytes_;
  }

  [[nodiscard]] std::uint64_t AsyncSendErrors() const noexcept override {
    return async_send_errors_.load(std::memory_order_relaxed);
  }

  /// 模拟异步完成回调里发生的错误（真实实现里来自 STALL / 传输失败）。
  void InjectAsyncSendError() noexcept {
    async_send_errors_.fetch_add(1, std::memory_order_relaxed);
  }

  // ---------------------------------------------------------------------------
  // 测试注入
  // ---------------------------------------------------------------------------

  /// 直接把一批「设备发来的帧」推进桥接层的 RX 队列。
  ///
  /// 走 BatchWrite，与真实的 libusb 回调路径一致。返回实际入队的帧数
  /// （队列满时会少于请求数，这正是要测的丢包路径）。
  [[nodiscard]] std::uint32_t InjectFromDevice(
      const std::vector<std::vector<std::byte>>& frames) {
    if (rx_ring_ == nullptr) {
      return 0;
    }
    auto batch = rx_ring_->BeginBatchWrite();
    std::uint32_t accepted = 0;
    std::uint64_t bytes = 0;
    for (const std::vector<std::byte>& frame : frames) {
      if (!batch.Push(frame)) {
        if (rx_counters_ != nullptr) {
          rx_counters_->AddDroppedFull();
        }
        continue;
      }
      ++accepted;
      bytes += frame.size();
    }
    if (accepted != 0 && rx_counters_ != nullptr) {
      rx_counters_->AddBatch(accepted, bytes);
    }
    return accepted;
  }

  /// 设置一次 SendFrames 最多吃下多少帧。设 0 可模拟传输池完全占满（背压）。
  ///
  /// 从 0 恢复为非 0 相当于「传输完成、槽位归还」，会唤醒等在
  /// WaitForSendCapacity 里的桥接层 TX 线程。
  void SetAcceptLimit(std::uint32_t limit) {
    accept_limit_.store(limit, std::memory_order_release);
    { const std::lock_guard<std::mutex> guard(wait_mutex_); }
    wait_cv_.notify_all();
  }

  void SetSendShouldFail(bool fail) noexcept {
    send_should_fail_.store(fail, std::memory_order_release);
  }

  // ---------------------------------------------------------------------------
  // 观测
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::uint64_t SentFrameCount() const noexcept {
    return sent_frames_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t SentByteCount() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return sent_bytes_;
  }

  [[nodiscard]] std::vector<std::vector<std::byte>> DrainSentToDevice() {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::vector<std::byte>> drained;
    drained.swap(sent_to_device_);
    return drained;
  }

  [[nodiscard]] bool ShutdownCalled() const noexcept {
    return shutdown_called_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool Receiving() const noexcept {
    return receiving_.load(std::memory_order_acquire);
  }

  /// WaitForSendCapacity 被调用的次数 —— 断言「桥接层确实在等而不是在丢」。
  [[nodiscard]] std::uint64_t WaitCalls() const noexcept {
    return wait_calls_.load(std::memory_order_relaxed);
  }

 private:
  std::uint32_t max_transfer_bytes_;

  FrameRing* rx_ring_ = nullptr;
  DirectionCounters* rx_counters_ = nullptr;

  mutable std::mutex mutex_;
  std::vector<std::vector<std::byte>> sent_to_device_;
  std::uint64_t sent_bytes_ = 0;

  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
  std::atomic<std::uint64_t> wait_calls_{0};

  std::atomic<std::uint64_t> sent_frames_{0};
  std::atomic<std::uint32_t> accept_limit_{0xFFFF'FFFFU};
  std::atomic<bool> send_should_fail_{false};
  std::atomic<bool> receiving_{false};
  std::atomic<bool> shutdown_called_{false};
  std::atomic<std::uint64_t> async_send_errors_{0};
};

}  // namespace tetherkitnext::testing
