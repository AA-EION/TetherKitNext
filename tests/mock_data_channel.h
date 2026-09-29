// In-memory RNDIS data channel, for driving the bridge layer offline.
//
// It plays the "USB device" side:
//   * After StartReceiving, InjectFromDevice can be used to inject "frames sent by the device", and an
//     internal producer thread pushes them into the bridge layer's RX queue -- simulating the role of the libusb event thread;
//   * SendFrames accepts "frames the host wants to send to the device", for tests to assert.
//
// This way the bridge layer's threading model, batching, backpressure and statistics can be fully tested on machines without a USB device,
// including concurrency correctness under TSan.
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

/// A data channel that simulates device behavior.
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
  // DataChannel implementation
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

    // Simulates the limited capacity of the transfer pool: accepts at most accept_limit_ frames at a time, and anything beyond is backpressure.
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
    // Wakes threads that may be waiting for capacity in WaitForSendCapacity (the same empty-critical-section idiom as the real implementation).
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

  /// Simulates errors happening in the asynchronous completion callback (in the real implementation they come from STALL / transfer failures).
  void InjectAsyncSendError() noexcept {
    async_send_errors_.fetch_add(1, std::memory_order_relaxed);
  }

  // ---------------------------------------------------------------------------
  // Test injection
  // ---------------------------------------------------------------------------

  /// Pushes a batch of "frames sent by the device" directly into the bridge layer's RX queue.
  ///
  /// Goes through BatchWrite, consistent with the real libusb callback path. Returns the number of frames actually enqueued
  /// (fewer than requested when the queue is full, which is exactly the drop path under test).
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

  /// Sets the maximum number of frames one SendFrames accepts. Setting 0 can simulate the transfer pool being completely full (backpressure).
  ///
  /// Going from 0 back to non-zero is equivalent to "transfer complete, slot returned", and wakes the bridge layer's TX thread
  /// waiting in WaitForSendCapacity.
  void SetAcceptLimit(std::uint32_t limit) {
    accept_limit_.store(limit, std::memory_order_release);
    { const std::lock_guard<std::mutex> guard(wait_mutex_); }
    wait_cv_.notify_all();
  }

  void SetSendShouldFail(bool fail) noexcept {
    send_should_fail_.store(fail, std::memory_order_release);
  }

  // ---------------------------------------------------------------------------
  // Observation
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

  /// The number of times WaitForSendCapacity was called -- asserts that "the bridge layer really is waiting rather than dropping".
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
