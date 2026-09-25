// 链路层后端抽象。
//
// 为什么需要抽象：BPF 需要 root 且需要真实的 feth 接口，开发机上跑不了。
// 把「收发原始以太帧」抽象成一个窄接口后，端到端测试与吞吐基准就能用内存
// loopback 后端在任何环境下跑。
//
// 性能取舍：接口用**虚函数**，但虚调用**不在每帧的粒度上** ——
// ReadFrames / WriteFrames 都是批量接口，一次虚调用处理几十上百帧，
// 分摊到每帧的开销远小于 1 ns，完全可以忽略。
// （如果做成每帧一次虚调用就不可接受了，那是刻意避开的设计。）
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/common/frame_ring.h"

namespace tetherkitnext::net {

/// 一批待发送的帧。
using FrameBatch = std::span<const FrameView>;

/// 一次批量读取的结果。
struct ReadBatch {
  /// 本批解出的帧，视图指向后端内部缓冲，在下一次 ReadFrames 前有效。
  std::span<const FrameView> frames;
  /// 内核侧累计丢包数（BPF 的 bs_drop）。用于背压告警。
  std::uint64_t kernel_drops = 0;
};

/// 一次批量写入的结果。
struct WriteResult {
  std::uint32_t frames_written = 0;
  std::uint64_t bytes_written = 0;
  /// 因单帧超长等原因被跳过的帧数。
  std::uint32_t frames_skipped = 0;
};

/// 收发原始以太帧的后端。
class LinkBackend {
 public:
  LinkBackend() = default;
  LinkBackend(const LinkBackend&) = delete;
  LinkBackend& operator=(const LinkBackend&) = delete;
  LinkBackend(LinkBackend&&) = delete;
  LinkBackend& operator=(LinkBackend&&) = delete;
  virtual ~LinkBackend() = default;

  /// 阻塞读取一批帧。
  ///
  /// 返回空批次是合法的（例如被信号打断），调用方应继续循环。
  /// 只有真正的错误才返回 Error。
  [[nodiscard]] virtual Result<ReadBatch> ReadFrames() = 0;

  /// 写出一批帧。尽最大努力写完；超长帧被跳过并计数。
  [[nodiscard]] virtual Result<WriteResult> WriteFrames(FrameBatch frames) = 0;

  /// 单帧长度上限（含 14 字节以太头）。
  [[nodiscard]] virtual std::uint32_t MaxFrameBytes() const noexcept = 0;

  /// 是否支持一次系统调用写多帧。用于日志与基准报告。
  [[nodiscard]] virtual bool SupportsBatchWrite() const noexcept = 0;

  /// 唤醒阻塞在 ReadFrames 里的线程，用于优雅停机。
  ///
  /// 必须可以从**其它线程**安全调用。
  virtual void Interrupt() noexcept = 0;
};

}  // namespace tetherkitnext::net
