// In-memory loopback link backend -- for offline tests and throughput benchmarks.
//
// Why it is a must: BpfLink needs root privileges + a real feth interface, while the development machine has neither
// root nor a USB device. After abstracting the link layer into LinkBackend, the end-to-end logic (the bridge layer's
// threading model, batching, backpressure, statistics) can be fully tested and benchmarked in any environment.
//
// Semantics:
//   * Frames written by WriteFrames enter the `sent` queue, for tests to assert "what the driver sent toward the host side";
//   * ReadFrames takes frames from the `inbound` queue, for tests to inject "what the host side sent";
//   * Both queues are bounded; when full they drop and count -- consistent with real BPF behavior.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "tetherkitnext/common/frame_ring.h"
#include "tetherkitnext/net/link_backend.h"

namespace tetherkitnext::net {

/// Configuration of the loopback backend.
struct LoopbackConfig {
  std::uint32_t max_frame_bytes = 1518;
  std::size_t inbound_capacity = 4096;   ///< Upper limit on frames waiting to be taken by ReadFrames.
  std::size_t sent_capacity = 4096;      ///< Upper limit on frames already written out by WriteFrames.
  std::size_t max_frames_per_batch = 256;
  /// Whether to claim support for batch writes. Tests need to cover both paths.
  bool report_batch_write = true;
};

/// Purely in-memory link backend.
///
/// Thread safety: ReadFrames may be called by only one thread, and WriteFrames by only one thread
/// (consistent with the LinkBackend contract); while PushInbound / DrainSent / Interrupt may be
/// called from any thread, protected internally by a mutex.
///
/// A mutex is deliberately used here instead of a lock-free queue: this class only serves the **injection side** of tests and benchmarks,
/// and is not on the data path under test; going lock-free would only add implementation complexity without testing anything more.
class LoopbackLink final : public LinkBackend {
 public:
  explicit LoopbackLink(const LoopbackConfig& config = {});

  // Copy and move are already deleted in the base class LinkBackend; they are restated explicitly here to satisfy static analysis.
  LoopbackLink(const LoopbackLink&) = delete;
  LoopbackLink& operator=(const LoopbackLink&) = delete;
  LoopbackLink(LoopbackLink&&) = delete;
  LoopbackLink& operator=(LoopbackLink&&) = delete;
  ~LoopbackLink() override = default;

  [[nodiscard]] Result<ReadBatch> ReadFrames() override;
  [[nodiscard]] Result<WriteResult> WriteFrames(FrameBatch frames) override;

  [[nodiscard]] std::uint32_t MaxFrameBytes() const noexcept override {
    return config_.max_frame_bytes;
  }

  [[nodiscard]] bool SupportsBatchWrite() const noexcept override {
    return config_.report_batch_write;
  }

  void Interrupt() noexcept override;

  // ---------------------------------------------------------------------------
  // Test injection and observation interfaces
  // ---------------------------------------------------------------------------

  /// Injects one frame "from the host side" for ReadFrames to take out. Returns false when the queue is full.
  [[nodiscard]] bool PushInbound(std::span<const std::byte> frame);

  /// Takes out and clears all frames "written by the driver to the host side".
  [[nodiscard]] std::vector<std::vector<std::byte>> DrainSent();

  /// Number of frames and bytes written out (cumulative, unaffected by DrainSent).
  [[nodiscard]] std::uint64_t TotalSentFrames() const noexcept {
    return total_sent_frames_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t TotalSentBytes() const noexcept {
    return total_sent_bytes_.load(std::memory_order_relaxed);
  }

  /// Number of injected frames dropped because the queue was full.
  [[nodiscard]] std::uint64_t InboundDrops() const noexcept {
    return inbound_drops_.load(std::memory_order_relaxed);
  }

  /// Whether Interrupt has been called.
  [[nodiscard]] bool Interrupted() const noexcept {
    return interrupted_.load(std::memory_order_acquire);
  }

  /// Makes WriteFrames return an error starting from the Nth call, for testing the error propagation path.
  void FailWritesAfter(std::uint32_t successful_calls) noexcept {
    fail_writes_after_.store(successful_calls, std::memory_order_relaxed);
  }

 private:
  LoopbackConfig config_;

  mutable std::mutex mutex_;
  /// Frames waiting to be read (FIFO).
  std::vector<std::vector<std::byte>> inbound_;
  /// Frames already written out.
  std::vector<std::vector<std::byte>> sent_;

  /// The view returned by ReadFrames must remain valid until the next call, so this batch's data stays here.
  std::vector<std::vector<std::byte>> read_storage_;
  std::vector<FrameView> read_views_;

  std::atomic<bool> interrupted_{false};
  std::atomic<std::uint64_t> total_sent_frames_{0};
  std::atomic<std::uint64_t> total_sent_bytes_{0};
  std::atomic<std::uint64_t> inbound_drops_{0};
  std::atomic<std::uint32_t> write_calls_{0};
  std::atomic<std::uint32_t> fail_writes_after_{0};
};

}  // namespace tetherkitnext::net
