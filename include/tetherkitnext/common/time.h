// Monotonic clock and timing utilities.
//
// Why not use std::chrono::steady_clock directly:
//   libc++'s steady_clock on Darwin goes through clock_gettime(CLOCK_MONOTONIC),
//   while clock_gettime_nsec_np(CLOCK_UPTIME_RAW) is cheaper (it reads mach absolute time directly,
//   with no timespec struct round trip, and is unaffected by NTP adjustments). Benchmarks and keepalive timers call it
//   tens of thousands of times per second, so the difference is worth it.
#pragma once

#include <cstdint>
#include <ctime>

namespace tetherkitnext {

/// Nanosecond timestamp type alias, making interface signatures self-explanatory.
using Nanos = std::uint64_t;

inline constexpr Nanos kNanosPerMicro = 1'000;
inline constexpr Nanos kNanosPerMilli = 1'000'000;
inline constexpr Nanos kNanosPerSecond = 1'000'000'000;

/// Monotonic nanoseconds since system boot. Unaffected by system time adjustments, and does not stop during sleep.
///
/// CLOCK_UPTIME_RAW is the cheapest time source on Darwin: no NTP correction, no struct conversion.
[[nodiscard]] inline Nanos MonotonicNanos() noexcept {
  return ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/// Stopwatch: starts timing on construction.
class Stopwatch {
 public:
  Stopwatch() noexcept : start_(MonotonicNanos()) {}

  /// Restarts timing.
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

/// Monotonically increasing periodic trigger, used for "do a keepalive / statistics report every N milliseconds".
///
/// Deliberately does not use a timer thread or a kqueue timer: the caller already wakes up periodically in its event loop,
/// and only needs a cheap "is it time yet?" check.
class PeriodicTimer {
 public:
  /// @param period Trigger period.
  /// @param start  Timing origin. **Passed in explicitly rather than reading the clock internally**, so that:
  ///               (1) unit tests can inject a deterministic time base (otherwise the moment the constructor reads
  ///                  is slightly later than the caller's `now`, making "now + period"
  ///                  not yet expired, and the test results unreproducible);
  ///               (2) a caller that creates multiple timers in one loop can share the same time base.
  ///               The default preserves the convenient "start timing on construction" usage.
  explicit PeriodicTimer(Nanos period, Nanos start = MonotonicNanos()) noexcept
      : period_(period), next_deadline_(start + period) {}

  /// Returns true when expired and advances the next deadline by one period.
  ///
  /// Uses "accumulated deadlines" rather than "reset to now + period", to avoid the timer as a whole
  /// drifting when the caller is delayed by scheduling. If it has fallen behind by more than a whole period, it aligns directly to after the current moment,
  /// avoiding a long string of expired triggers being replayed after waking up.
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

  /// Nanoseconds remaining until the next expiry; returns 0 if already expired. Used by the event loop to compute select/kevent timeouts.
  [[nodiscard]] Nanos RemainingNanos(Nanos now) const noexcept {
    return now >= next_deadline_ ? 0 : next_deadline_ - now;
  }

  [[nodiscard]] Nanos Period() const noexcept { return period_; }

 private:
  Nanos period_;
  Nanos next_deadline_;
};

}  // namespace tetherkitnext
