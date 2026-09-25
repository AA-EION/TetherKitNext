// 单调时钟与计时工具。
//
// 为什么不直接用 std::chrono::steady_clock：
//   libc++ 的 steady_clock 在 Darwin 上走 clock_gettime(CLOCK_MONOTONIC)，
//   而 clock_gettime_nsec_np(CLOCK_UPTIME_RAW) 更便宜（直接读 mach 绝对时间，
//   不做 timespec 结构体往返，也不受 NTP 调整影响）。基准与保活定时器每秒会调
//   用几万次，这个差别值得。
#pragma once

#include <cstdint>
#include <ctime>

namespace tetherkitnext {

/// 纳秒时间戳类型别名，让接口签名自解释。
using Nanos = std::uint64_t;

inline constexpr Nanos kNanosPerMicro = 1'000;
inline constexpr Nanos kNanosPerMilli = 1'000'000;
inline constexpr Nanos kNanosPerSecond = 1'000'000'000;

/// 自系统启动以来的单调纳秒数。不受系统时间调整影响，不在睡眠时停止。
///
/// CLOCK_UPTIME_RAW 是 Darwin 上最便宜的时间源：无 NTP 校正、无结构体转换。
[[nodiscard]] inline Nanos MonotonicNanos() noexcept {
  return ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/// 秒表：构造即开始计时。
class Stopwatch {
 public:
  Stopwatch() noexcept : start_(MonotonicNanos()) {}

  /// 重新开始计时。
  void Reset() noexcept { start_ = MonotonicNanos(); }

  [[nodiscard]] Nanos Elapsed() const noexcept { return MonotonicNanos() - start_; }

  [[nodiscard]] double ElapsedMillis() const noexcept {
    return static_cast<double>(Elapsed()) / static_cast<double>(kNanosPerMilli);
  }

  [[nodiscard]] double ElapsedSeconds() const noexcept {
    return static_cast<double>(Elapsed()) / static_cast<double>(kNanosPerSecond);
  }

 private:
  Nanos start_;
};

/// 单调递增的周期性触发判定器，用于「每 N 毫秒做一次保活 / 统计报告」。
///
/// 刻意不用定时器线程或 kqueue 定时器：调用方本来就在事件循环里定期醒来，
/// 只需要一个便宜的「时候到了吗」判断。
class PeriodicTimer {
 public:
  /// @param period 触发周期。
  /// @param start  计时起点。**显式传入而不是内部读时钟**，这样：
  ///               ① 单元测试可以注入确定的时间基准（否则构造函数自己读到的
  ///                  时刻会比调用方手里的 `now` 略晚，导致「now + period」
  ///                  反而还没到期，测试结果不可复现）；
  ///               ② 调用方在一个循环里创建多个定时器时能共用同一个时间基准。
  ///               默认值保留「构造即开始计时」的便利写法。
  explicit PeriodicTimer(Nanos period, Nanos start = MonotonicNanos()) noexcept
      : period_(period), next_deadline_(start + period) {}

  /// 到期则返回 true 并把下一次期限推进一个周期。
  ///
  /// 采用「累加期限」而非「重置为 now + period」，避免调用方被调度延迟时
  /// 定时器整体漂移。若已落后超过一整个周期，直接对齐到当前时刻之后，
  /// 避免醒来后连续补发一大串过期触发。
  [[nodiscard]] bool Expired(Nanos now) noexcept {
    if (now < next_deadline_) {
      return false;
    }
    next_deadline_ += period_;
    if (next_deadline_ <= now) {
      next_deadline_ = now + period_;
    }
    return true;
  }

  [[nodiscard]] bool Expired() noexcept { return Expired(MonotonicNanos()); }

  /// 距下次到期还剩多少纳秒；已到期返回 0。供事件循环计算 select/kevent 超时。
  [[nodiscard]] Nanos RemainingNanos(Nanos now) const noexcept {
    return now >= next_deadline_ ? 0 : next_deadline_ - now;
  }

  [[nodiscard]] Nanos Period() const noexcept { return period_; }

 private:
  Nanos period_;
  Nanos next_deadline_;
};

}  // namespace tetherkitnext
