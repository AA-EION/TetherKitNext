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

// Logging configuration is a process-wide single state, naturally a mutable global; after using atomics for thread safety,
// splitting it into a singleton class would only add an indirection layer with no real benefit, so the related checks are exempted wholesale here.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)

/// Runtime log floor. An atomic rather than a mutex: it is read in the check branch of every log line.
std::atomic<LogLevel> g_runtime_level{LogLevel::kInfo};

/// Whether to output ANSI colors. Lazily initialized to `isatty(STDERR_FILENO)`.
std::atomic<int> g_color_enabled{-1};

/// Protects the whole-line output to stderr, avoiding interleaving of multi-threaded logs.
/// Logging is not on the data hot path, so lock contention can be ignored.
std::mutex& OutputMutex() {
  static std::mutex mutex;
  return mutex;
}

/// The log sink installed by the host. sink and user must be updated **as a pair**, so they are protected by OutputMutex
/// rather than two independent atomics -- two atomics cannot be updated atomically together, and a mismatched combination could be read.
LogSink g_log_sink = nullptr;
void* g_log_sink_user = nullptr;

/// Thread name. thread_local rather than querying pthread_getname_np: the latter is a system call.
constexpr std::size_t kThreadNameCapacity = 16;
thread_local std::array<char, kThreadNameCapacity> t_thread_name{};

std::string_view CurrentThreadName() {
  if (t_thread_name[0] == '\0') {
    return "main";
  }
  return {t_thread_name.data(), std::strlen(t_thread_name.data())};
}

/// Level labels and ANSI color codes.
///
/// Deliberately `const char*` rather than `std::string_view`: these values are fed directly to `%s`,
/// and the `data()` of an empty string_view is nullptr, which is undefined behavior when passed to printf.
struct LevelStyle {
  const char* label;
  const char* color;
};

LevelStyle StyleFor(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return {"TRACE", "\033[90m"};  // bright black (gray)
    case LogLevel::kDebug:
      return {"DEBUG", "\033[36m"};  // cyan
    case LogLevel::kInfo:
      return {"INFO ", "\033[32m"};  // green
    case LogLevel::kWarn:
      return {"WARN ", "\033[33m"};  // yellow
    case LogLevel::kError:
      return {"ERROR", "\033[31m"};  // red
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

/// Formats the current wall-clock time as `HH:MM:SS.mmm`.
///
/// Wall time is used rather than monotonic time: logs are for humans to read and need to line up with other system logs.
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
  // Sync to the kernel, so lldb / Instruments / `sample` can also see a meaningful thread name.
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

  // Output the whole line with a single fprintf, together with the mutex to guarantee that lines do not interleave.
  // std::format is not used, to avoid the possibility of an exception here too -- the logging path must be noexcept.
  // file / thread_name / message are string_views that may lack a terminator,
  // so `%.*s` is used uniformly to pass the length explicitly.
  const std::lock_guard<std::mutex> guard(OutputMutex());
  std::fprintf(stderr, "%s%s%s %s [%-10.*s] %.*s:%u  %.*s\n", color_on, style.label, color_off,
               timestamp.data(), static_cast<int>(thread_name.size()), thread_name.data(),
               static_cast<int>(file.size()), file.data(), line, static_cast<int>(message.size()),
               message.data());

  // Hand off to the host. Calling inside the lock is deliberate: this way the line order the host sees is exactly the same as on stderr.
  // The cost is that the sink must never log again (a self-wait deadlock); this constraint is written in logging.h.
  if (g_log_sink != nullptr) {
    g_log_sink(level, thread_name, message, g_log_sink_user);
  }
}

}  // namespace detail
}  // namespace tetherkitnext
