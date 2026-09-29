// Log capture: buffers the library's internal log lines for the GUI to poll and take away.
//
// Why not pass through to Swift via a callback: logs come up from the libusb event thread, the control thread and the two data-path
// threads, and are produced **inside** the log mutex. Running a Swift closure in that context
// would require cross-thread marshaling and would also carry the landmine of "the closure accidentally logs again -> self-wait deadlock".
// Buffer + polling eliminates both problems at once.
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "capi_support.h"
#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/logging.h"

namespace {

using tetherkitnext::capi::CopyText;
using tetherkitnext::capi::WallNanos;

/// Ring buffer capacity.
///
/// Basis for 256: the GUI pulls every 500 ms, while the library writes only one statistics line every 5 seconds in steady state.
/// 256 entries are enough to withstand the dense burst of logs from the startup sequence (about 30 lines) and any sudden warnings,
/// while keeping resident memory at 256 x 552 B ~= 138 KiB.
constexpr std::size_t kLogRingCapacity = 256;

/// A fixed-size ring buffer. When full it drops the **oldest** -- when the host is stuck, the latest scene is more useful than the beginning.
class LogRing {
 public:
  void Push(tk_log_level_t level, std::string_view thread_name,
            std::string_view message) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == kLogRingCapacity) {
      // Overwrite the oldest entry: advance the read pointer, which logically amounts to dropping it.
      head_ = (head_ + 1) % kLogRingCapacity;
      --count_;
      ++dropped_;
    }
    tk_log_record_t& record = records_[(head_ + count_) % kLogRingCapacity];
    record.level = static_cast<std::int32_t>(level);
    record.wall_nanos = WallNanos();
    CopyText(record.thread, thread_name);
    CopyText(record.message, message);
    ++count_;
  }

  std::size_t Drain(tk_log_record_t* out_records, std::size_t capacity,
                    std::uint64_t* out_dropped) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (out_dropped != nullptr) {
      *out_dropped = dropped_;
      dropped_ = 0;
    }
    std::size_t taken = 0;
    while (taken < capacity && count_ > 0) {
      if (out_records != nullptr) {
        out_records[taken] = records_[head_];
      }
      head_ = (head_ + 1) % kLogRingCapacity;
      --count_;
      ++taken;
    }
    return taken;
  }

  void Clear() noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    head_ = 0;
    count_ = 0;
    dropped_ = 0;
  }

 private:
  std::mutex mutex_;
  std::array<tk_log_record_t, kLogRingCapacity> records_{};
  /// Index of the next record to be read.
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::uint64_t dropped_ = 0;
};

/// Process-wide singleton. A function-local static rather than a global object, to avoid static initialization order problems --
/// the log sink may be triggered during the static construction of any translation unit.
LogRing& Ring() noexcept {
  static LogRing ring;
  return ring;
}

/// The callback installed for tetherkitnext::SetLogSink.
///
/// It runs inside the log mutex, so the implementation may only do "copy + take one lock of its own",
/// and must never log again (a self-wait deadlock).
void SinkTrampoline(tetherkitnext::LogLevel level, std::string_view thread_name,
                    std::string_view message, void* /*user*/) noexcept {
  Ring().Push(static_cast<tk_log_level_t>(level), thread_name, message);
}

}  // namespace

void tk_set_log_level(int32_t level) {
  if (level < TK_LOG_TRACE || level > TK_LOG_OFF) {
    return;
  }
  tetherkitnext::SetLogLevel(static_cast<tetherkitnext::LogLevel>(level));
}

void tk_enable_log_capture(bool enabled) {
  if (enabled) {
    tetherkitnext::SetLogSink(&SinkTrampoline, nullptr);
    return;
  }

  // The order matters: **first** remove the log sink, **then** clear the buffer.
  //
  // Reversed, a thread that was logging before the removal would stuff a new record into the just-cleared buffer,
  // and after capture is turned off a few leftovers would remain instead.
  //
  // Also, LogRing's lock is deliberately not held here while calling SetLogSink -- SetLogSink needs to take the log
  // mutex, while the log sink path is "log lock -> LogRing lock", and holding locks in the reverse order would form a lock-order inversion.
  tetherkitnext::SetLogSink(nullptr, nullptr);
  Ring().Clear();
}

size_t tk_drain_logs(tk_log_record_t* out_records, size_t capacity, uint64_t* out_dropped) {
  if (out_dropped != nullptr) {
    *out_dropped = 0;
  }
  if (capacity == 0) {
    return 0;
  }
  return Ring().Drain(out_records, capacity, out_dropped);
}
