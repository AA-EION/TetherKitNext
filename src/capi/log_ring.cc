// 日志捕获：把库内部的日志行缓冲起来，供 GUI 轮询取走。
//
// 为什么不用回调直通 Swift：日志会从 libusb 事件线程、控制线程、两条数据路径
// 线程上来，而且是在日志互斥锁**内部**产生的。让 Swift 闭包在那个上下文里跑，
// 既要跨线程 marshal，又有「闭包里不小心又打了一条日志 → 自等死锁」的雷。
// 缓冲 + 轮询把这两个问题一次性消掉。
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

/// 环形缓冲容量。
///
/// 取 256 的依据：GUI 每 500 ms 拉一次，而库在稳态下每 5 秒才打一行统计。
/// 256 条足以扛住启动序列那一波密集日志（约 30 行）与任何突发告警，
/// 同时把常驻内存控制在 256 × 552 B ≈ 138 KiB。
constexpr std::size_t kLogRingCapacity = 256;

/// 定长环形缓冲。写满时丢**最旧**的 —— 宿主卡住时，最新的现场比开头有用。
class LogRing {
 public:
  void Push(tk_log_level_t level, std::string_view thread_name,
            std::string_view message) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == kLogRingCapacity) {
      // 覆盖最旧的一条：读指针前移，逻辑上等于把它丢掉。
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
  /// 下一条待读记录的下标。
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::uint64_t dropped_ = 0;
};

/// 进程级单例。用函数内静态量而非全局对象，避免静态初始化顺序问题 ——
/// 日志汇可能在任何翻译单元的静态构造期间就被触发。
LogRing& Ring() noexcept {
  static LogRing ring;
  return ring;
}

/// 安装给 tetherkitnext::SetLogSink 的回调。
///
/// 它运行在日志互斥锁内部，因此实现里只允许「拷贝 + 加一把自己的锁」，
/// 绝不能再打日志（会自等死锁）。
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

  // 顺序很重要：**先**摘掉日志汇，**再**清空缓冲。
  //
  // 反过来的话，摘除之前正在打日志的线程会把新记录塞进刚清空的缓冲里，
  // 关闭捕获后反而留着几条残留。
  //
  // 另外，这里刻意不持有 LogRing 的锁去调 SetLogSink —— SetLogSink 要拿日志
  // 互斥锁，而日志汇路径是「日志锁 → LogRing 锁」，反向持锁会构成锁序倒置。
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
