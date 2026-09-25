// 数据路径统计计数器。
//
// 设计要点：
//   * 热路径上的计数器**不能**是共享的 std::atomic —— 两个线程对同一个原子做
//     fetch_add，在 25k~80k pps 下会把那条缓存行打成乒乓球。
//   * 因此采用「每方向一份私有计数器 + 读取时快照」的结构：写方是单线程，
//     用 relaxed store 更新（编译后就是一条普通 str 指令）；观测线程用
//     relaxed load 读快照，允许读到轻微不一致的一组数值 —— 统计不需要
//     强一致性。
//   * 用 std::atomic<uint64_t> + relaxed 而非裸 uint64_t，是为了让
//     ThreadSanitizer 不把「观测线程读、工作线程写」判成数据竞争。
//     relaxed 原子在 arm64 上就是普通的 ldr/str，无额外开销。
#pragma once

#include <atomic>
#include <cstdint>

#include "tetherkitnext/common/cache.h"
#include "tetherkitnext/common/time.h"

namespace tetherkitnext {

/// 单向（RX 或 TX）的数据路径计数器。
///
/// 只允许**一个**线程调用 Add* 方法；任意线程可以调用 Snapshot()。
struct alignas(kCacheLineSize) DirectionCounters {
  std::atomic<std::uint64_t> frames{0};        ///< 成功搬运的帧数。
  std::atomic<std::uint64_t> bytes{0};         ///< 成功搬运的字节数（以太帧净荷）。
  std::atomic<std::uint64_t> dropped_full{0};  ///< 因下游队列满而丢弃的帧数。
  std::atomic<std::uint64_t> dropped_oversize{0};  ///< 因超过单帧上限而丢弃的帧数。
  std::atomic<std::uint64_t> dropped_malformed{0};  ///< 因格式非法而丢弃的帧数。
  std::atomic<std::uint64_t> io_errors{0};     ///< 底层 I/O 失败次数（write/传输错误）。
  std::atomic<std::uint64_t> batches{0};       ///< 批次数，用来算平均批大小。

  /// 记录一帧成功搬运。这是最热的一行代码，刻意保持只有两次 relaxed 累加。
  void AddFrame(std::uint32_t frame_bytes) noexcept {
    Bump(frames, 1);
    Bump(bytes, frame_bytes);
  }

  void AddBatch(std::uint64_t frame_count, std::uint64_t byte_count) noexcept {
    Bump(frames, frame_count);
    Bump(bytes, byte_count);
    Bump(batches, 1);
  }

  void AddDroppedFull(std::uint64_t count = 1) noexcept { Bump(dropped_full, count); }

  void AddDroppedOversize(std::uint64_t count = 1) noexcept { Bump(dropped_oversize, count); }

  void AddDroppedMalformed(std::uint64_t count = 1) noexcept { Bump(dropped_malformed, count); }

  void AddIoError(std::uint64_t count = 1) noexcept { Bump(io_errors, count); }

 private:
  /// relaxed 读-改-写。因为只有单一写线程，这里不需要 fetch_add 的原子性，
  /// load+store 即可，能省掉 arm64 上的 LSE 原子指令（ldadd）。
  static void Bump(std::atomic<std::uint64_t>& counter, std::uint64_t delta) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
  }
};

/// DirectionCounters 的普通（非原子）快照，便于做差值与格式化。
struct DirectionSnapshot {
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  std::uint64_t dropped_full = 0;
  std::uint64_t dropped_oversize = 0;
  std::uint64_t dropped_malformed = 0;
  std::uint64_t io_errors = 0;
  std::uint64_t batches = 0;

  [[nodiscard]] std::uint64_t TotalDropped() const noexcept {
    return dropped_full + dropped_oversize + dropped_malformed;
  }

  /// 逐字段相减，得到两次采样之间的增量。
  [[nodiscard]] DirectionSnapshot operator-(const DirectionSnapshot& earlier) const noexcept {
    return DirectionSnapshot{
        .frames = frames - earlier.frames,
        .bytes = bytes - earlier.bytes,
        .dropped_full = dropped_full - earlier.dropped_full,
        .dropped_oversize = dropped_oversize - earlier.dropped_oversize,
        .dropped_malformed = dropped_malformed - earlier.dropped_malformed,
        .io_errors = io_errors - earlier.io_errors,
        .batches = batches - earlier.batches,
    };
  }
};

[[nodiscard]] inline DirectionSnapshot Snapshot(const DirectionCounters& counters) noexcept {
  return DirectionSnapshot{
      .frames = counters.frames.load(std::memory_order_relaxed),
      .bytes = counters.bytes.load(std::memory_order_relaxed),
      .dropped_full = counters.dropped_full.load(std::memory_order_relaxed),
      .dropped_oversize = counters.dropped_oversize.load(std::memory_order_relaxed),
      .dropped_malformed = counters.dropped_malformed.load(std::memory_order_relaxed),
      .io_errors = counters.io_errors.load(std::memory_order_relaxed),
      .batches = counters.batches.load(std::memory_order_relaxed),
  };
}

/// 双向数据路径的全部计数器。
///
/// RX = 设备 → 主机（USB bulk IN → BPF write）
/// TX = 主机 → 设备（BPF read → USB bulk OUT）
struct PathCounters {
  DirectionCounters rx;
  DirectionCounters tx;
};

/// 一段时间窗口内的速率，用于周期性报告。
struct RateReport {
  double seconds = 0.0;
  double rx_pps = 0.0;
  double rx_mbps = 0.0;
  double tx_pps = 0.0;
  double tx_mbps = 0.0;
  std::uint64_t rx_dropped = 0;
  std::uint64_t tx_dropped = 0;
  double rx_avg_batch = 0.0;
  double tx_avg_batch = 0.0;
};

/// 按固定周期把计数器差值换算成速率。
class RateSampler {
 public:
  RateSampler() : last_nanos_(MonotonicNanos()) {}

  /// 采样一次，返回自上次采样以来的速率。
  [[nodiscard]] RateReport Sample(const PathCounters& counters) noexcept {
    const Nanos now = MonotonicNanos();
    const DirectionSnapshot rx_now = Snapshot(counters.rx);
    const DirectionSnapshot tx_now = Snapshot(counters.tx);

    const double seconds =
        static_cast<double>(now - last_nanos_) / static_cast<double>(kNanosPerSecond);
    const DirectionSnapshot rx_delta = rx_now - last_rx_;
    const DirectionSnapshot tx_delta = tx_now - last_tx_;

    last_nanos_ = now;
    last_rx_ = rx_now;
    last_tx_ = tx_now;

    if (seconds <= 0.0) {
      return RateReport{};
    }
    return RateReport{
        .seconds = seconds,
        .rx_pps = static_cast<double>(rx_delta.frames) / seconds,
        .rx_mbps = BytesToMegabitsPerSecond(rx_delta.bytes, seconds),
        .tx_pps = static_cast<double>(tx_delta.frames) / seconds,
        .tx_mbps = BytesToMegabitsPerSecond(tx_delta.bytes, seconds),
        .rx_dropped = rx_delta.TotalDropped(),
        .tx_dropped = tx_delta.TotalDropped(),
        .rx_avg_batch = AverageBatch(rx_delta),
        .tx_avg_batch = AverageBatch(tx_delta),
    };
  }

 private:
  static double BytesToMegabitsPerSecond(std::uint64_t bytes, double seconds) noexcept {
    return static_cast<double>(bytes) * 8.0 / 1'000'000.0 / seconds;
  }

  static double AverageBatch(const DirectionSnapshot& delta) noexcept {
    return delta.batches == 0 ? 0.0
                              : static_cast<double>(delta.frames) / static_cast<double>(delta.batches);
  }

  Nanos last_nanos_;
  DirectionSnapshot last_rx_;
  DirectionSnapshot last_tx_;
};

}  // namespace tetherkitnext
