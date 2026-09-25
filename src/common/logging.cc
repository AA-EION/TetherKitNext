#include "tetherkitnext/common/logging.h"

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace tetherkitnext {
namespace {

// 日志配置是进程级的单一状态，天然是可变全局量；用原子保证线程安全后，
// 再拆成单例类只会增加间接层而无实质收益，故此处整体豁免相关检查。
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)

/// 运行期日志下限。用原子而非 mutex：读取发生在每条日志的判断分支上。
std::atomic<LogLevel> g_runtime_level{LogLevel::kInfo};

/// 是否输出 ANSI 颜色。惰性初始化为 `isatty(STDERR_FILENO)`。
std::atomic<int> g_color_enabled{-1};

/// 保护 stderr 的整行输出，避免多线程日志交错。
/// 日志不在数据热路径上，锁竞争可以忽略。
std::mutex& OutputMutex() {
  static std::mutex mutex;
  return mutex;
}

/// 宿主安装的日志汇。sink 与 user 必须**成对**更新，所以用 OutputMutex 保护
/// 而不是两个独立的原子 —— 两个原子无法一起原子更新，会读到错配的组合。
LogSink g_log_sink = nullptr;
void* g_log_sink_user = nullptr;

/// 线程名。thread_local 而非查询 pthread_getname_np：后者是系统调用。
constexpr std::size_t kThreadNameCapacity = 16;
thread_local std::array<char, kThreadNameCapacity> t_thread_name{};

std::string_view CurrentThreadName() {
  if (t_thread_name[0] == '\0') {
    return "main";
  }
  return {t_thread_name.data(), std::strlen(t_thread_name.data())};
}

/// 级别标签与 ANSI 颜色码。
///
/// 刻意用 `const char*` 而非 `std::string_view`：这些值会直接喂给 `%s`，
/// 而空 string_view 的 `data()` 是 nullptr，传给 printf 是未定义行为。
struct LevelStyle {
  const char* label;
  const char* color;
};

LevelStyle StyleFor(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return {"TRACE", "\033[90m"};  // 亮黑（灰）
    case LogLevel::kDebug:
      return {"DEBUG", "\033[36m"};  // 青
    case LogLevel::kInfo:
      return {"INFO ", "\033[32m"};  // 绿
    case LogLevel::kWarn:
      return {"WARN ", "\033[33m"};  // 黄
    case LogLevel::kError:
      return {"ERROR", "\033[31m"};  // 红
    case LogLevel::kOff:
      break;
  }
  return {"?????", ""};
}

bool ColorEnabled() {
  int cached = g_color_enabled.load(std::memory_order_relaxed);
  if (cached < 0) {
    cached = ::isatty(STDERR_FILENO) != 0 ? 1 : 0;
    g_color_enabled.store(cached, std::memory_order_relaxed);
  }
  return cached != 0;
}

/// 把当前墙上时间格式化成 `HH:MM:SS.mmm`。
///
/// 用墙上时间而非单调时间：日志是给人看的，需要能和其他系统日志对齐。
void FormatTimestamp(std::array<char, 16>& out) {
  ::timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  ::tm local{};
  ::localtime_r(&ts.tv_sec, &local);
  const long millis = ts.tv_nsec / 1'000'000;
  std::snprintf(out.data(), out.size(), "%02d:%02d:%02d.%03ld", local.tm_hour, local.tm_min,
                local.tm_sec, millis);
}

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

void SetLogLevel(LogLevel level) noexcept {
  g_runtime_level.store(level, std::memory_order_relaxed);
}

LogLevel GetLogLevel() noexcept {
  return g_runtime_level.load(std::memory_order_relaxed);
}

void SetLogColorEnabled(bool enabled) noexcept {
  g_color_enabled.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void SetLogSink(LogSink sink, void* user) noexcept {
  const std::lock_guard<std::mutex> guard(OutputMutex());
  g_log_sink = sink;
  g_log_sink_user = user;
}

void SetCurrentThreadName(std::string_view name) noexcept {
  const std::size_t copy_len = std::min(name.size(), kThreadNameCapacity - 1);
  std::memcpy(t_thread_name.data(), name.data(), copy_len);
  t_thread_name[copy_len] = '\0';
  // 同步给内核，使 lldb / Instruments / `sample` 也能看到有意义的线程名。
  ::pthread_setname_np(t_thread_name.data());
}

namespace detail {

bool IsLogLevelEnabled(LogLevel level) noexcept {
  return level >= g_runtime_level.load(std::memory_order_relaxed);
}

void EmitLogLine(LogLevel level, std::string_view file, unsigned line,
                 std::string_view message) noexcept {
  const LevelStyle style = StyleFor(level);
  std::array<char, 16> timestamp{};
  FormatTimestamp(timestamp);

  const bool color = ColorEnabled();
  const char* color_on = color ? style.color : "";
  const char* color_off = color ? "\033[0m" : "";
  const std::string_view thread_name = CurrentThreadName();

  // 用一次 fprintf 输出整行，配合互斥锁保证行不交错。
  // 不用 std::format 是为了避免这里也可能抛异常 —— 日志路径必须 noexcept。
  // file / thread_name / message 是可能不带终止符的 string_view，
  // 因此统一用 `%.*s` 显式传长度。
  const std::lock_guard<std::mutex> guard(OutputMutex());
  std::fprintf(stderr, "%s%s%s %s [%-10.*s] %.*s:%u  %.*s\n", color_on, style.label, color_off,
               timestamp.data(), static_cast<int>(thread_name.size()), thread_name.data(),
               static_cast<int>(file.size()), file.data(), line, static_cast<int>(message.size()),
               message.data());

  // 转交给宿主。在锁内调用是刻意的：这样宿主看到的行序与 stderr 完全一致。
  // 代价是 sink 里绝不能再打日志（会自等死锁），该约束写在 logging.h 上。
  if (g_log_sink != nullptr) {
    g_log_sink(level, thread_name, message, g_log_sink_user);
  }
}

}  // namespace detail
}  // namespace tetherkitnext
