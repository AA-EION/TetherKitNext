// 内存 loopback 链路后端 —— 用于离线测试与吞吐基准。
//
// 为什么必须有它：BpfLink 需要 root 权限 + 真实的 feth 接口，而开发机上既没有
// root 也没有 USB 设备。把链路层抽象成 LinkBackend 之后，端到端逻辑（桥接层的
// 线程模型、批处理、背压、统计）就能在任何环境下被完整测试和基准。
//
// 语义：
//   * WriteFrames 写入的帧进入 `sent` 队列，供测试断言「驱动往主机侧发了什么」；
//   * ReadFrames 从 `inbound` 队列取帧，供测试注入「主机侧发来了什么」；
//   * 两个队列都是有界的，满了就丢弃并计数 —— 与真实 BPF 的行为一致。
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "tetherkitnext/common/frame_ring.h"
#include "tetherkitnext/net/link_backend.h"

namespace tetherkitnext::net {

/// loopback 后端的配置。
struct LoopbackConfig {
  std::uint32_t max_frame_bytes = 1518;
  std::size_t inbound_capacity = 4096;   ///< 待被 ReadFrames 取走的帧数上限。
  std::size_t sent_capacity = 4096;      ///< 已被 WriteFrames 写出的帧数上限。
  std::size_t max_frames_per_batch = 256;
  /// 是否宣称支持批量写。测试里两种路径都要覆盖。
  bool report_batch_write = true;
};

/// 纯内存的链路后端。
///
/// 线程安全：ReadFrames 只允许一个线程调用，WriteFrames 只允许一个线程调用
/// （与 LinkBackend 的契约一致）；而 PushInbound / DrainSent / Interrupt 可以
/// 从任意线程调用，内部用互斥锁保护。
///
/// 这里刻意用互斥锁而不是无锁队列：本类只服务测试与基准的**注入侧**，
/// 不在被测的数据路径上；无锁化只会增加实现复杂度却测不出更多东西。
class LoopbackLink final : public LinkBackend {
 public:
  explicit LoopbackLink(const LoopbackConfig& config = {});

  // 拷贝与移动已在基类 LinkBackend 中删除，这里显式重申以满足静态检查。
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
  // 测试注入与观测接口
  // ---------------------------------------------------------------------------

  /// 注入一帧「来自主机侧」的数据，供 ReadFrames 取出。队列满返回 false。
  [[nodiscard]] bool PushInbound(std::span<const std::byte> frame);

  /// 取出并清空「驱动写给主机侧」的全部帧。
  [[nodiscard]] std::vector<std::vector<std::byte>> DrainSent();

  /// 已写出的帧数与字节数（累计，不受 DrainSent 影响）。
  [[nodiscard]] std::uint64_t TotalSentFrames() const noexcept {
    return total_sent_frames_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t TotalSentBytes() const noexcept {
    return total_sent_bytes_.load(std::memory_order_relaxed);
  }

  /// 因队列满而丢弃的注入帧数。
  [[nodiscard]] std::uint64_t InboundDrops() const noexcept {
    return inbound_drops_.load(std::memory_order_relaxed);
  }

  /// 是否已被 Interrupt。
  [[nodiscard]] bool Interrupted() const noexcept {
    return interrupted_.load(std::memory_order_acquire);
  }

  /// 让 WriteFrames 从第 N 次调用起返回错误，用于测试错误传播路径。
  void FailWritesAfter(std::uint32_t successful_calls) noexcept {
    fail_writes_after_.store(successful_calls, std::memory_order_relaxed);
  }

 private:
  LoopbackConfig config_;

  mutable std::mutex mutex_;
  /// 待读取的帧（FIFO）。
  std::vector<std::vector<std::byte>> inbound_;
  /// 已写出的帧。
  std::vector<std::vector<std::byte>> sent_;

  /// ReadFrames 返回的视图必须在下次调用前保持有效，因此本批数据留在这里。
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
