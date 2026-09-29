// Lightweight leveled logging.
//
// Design constraints:
//   1. **Forbidden on the data hot path.** The Trace/Debug levels are removed wholesale by the
//      preprocessor in Release builds (even argument evaluation does not happen), so writing TETHERKITNEXT_TRACE
//      on a hot path is safe; but Info and above really do formatting and take a lock to write stderr, so they are forbidden on the hot path.
//   2. Thread-safe: multiple threads writing stderr at once would interleave, so whole lines are written under a mutex.
//      Logging is not on the hot path, so lock contention does not matter.
//   3. No third-party logging library is introduced: only "time + level + thread name + location + message" is needed,
//      and std::format is enough.
#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "tetherkitnext/common/i18n.h"

namespace tetherkitnext {

enum class LogLevel : std::uint8_t {
  kTrace = 0,  ///< Per-frame level detail; removed at compile time in Release builds.
  kDebug = 1,  ///< Protocol interaction detail (every RNDIS control message); removed at compile time in Release.
  kInfo = 2,   ///< Lifecycle events (device connected, state machine transitions, statistics reports).
  kWarn = 3,   ///< Recoverable anomalies (USB stall, BPF packet drops, keepalive timeout retries).
  kError = 4,  ///< Unrecoverable errors.
  kOff = 5,    ///< Everything off.
};

/// Compile-time log floor. Log calls below this level are removed wholesale by the preprocessor.
///
/// In Release builds (NDEBUG), Trace/Debug are removed, ensuring that TETHERKITNEXT_TRACE on the hot path
/// does not even evaluate its arguments.
#ifndef TETHERKITNEXT_COMPILED_MIN_LOG_LEVEL
#ifdef NDEBUG
#define TETHERKITNEXT_COMPILED_MIN_LOG_LEVEL 2  // kInfo
#else
#define TETHERKITNEXT_COMPILED_MIN_LOG_LEVEL 0  // kTrace
#endif
#endif

/// Runtime log floor, defaulting to kInfo.
void SetLogLevel(LogLevel level) noexcept;

LogLevel GetLogLevel() noexcept;

/// Whether to enable colored output. By default enabled only when stderr is a tty.
void SetLogColorEnabled(bool enabled) noexcept;

/// Gives the current thread a short name that appears in log lines, making it easy to tell usb-event / bpf-rx / bpf-tx apart.
/// Also calls pthread_setname_np so it is visible in Instruments / lldb too.
void SetCurrentThreadName(std::string_view name) noexcept;

/// Log sink: **additionally** forwards already-formatted log lines to the host (the GUI needs to show logs in the interface,
/// while the logs themselves only go to stderr). stderr output is unaffected; the two run in parallel.
///
/// * Three hard constraints when implementing a log sink *
///   1. It is called on **any thread** (including the libusb event thread);
///   2. It is called **inside** the log mutex -- so the implementation **must never log again**
///      (std::mutex is not reentrant and would deadlock on the spot);
///   3. It must never do blocking I/O -- it would hold up all threads that are currently logging.
///
/// The only reasonable implementation satisfying these three is "copy into a fixed-size ring buffer that the host polls and takes away".
using LogSink = void (*)(LogLevel level, std::string_view thread_name, std::string_view message,
                         void* user) noexcept;

/// Installs / uninstalls the log sink. Pass nullptr to uninstall. Thread-safe.
void SetLogSink(LogSink sink, void* user) noexcept;

namespace detail {

[[nodiscard]] bool IsLogLevelEnabled(LogLevel level) noexcept;

/// Outputs a whole log line. `message` is already formatted.
void EmitLogLine(LogLevel level, std::string_view file, unsigned line,
                 std::string_view message) noexcept;

/// Cuts the file name out of __FILE__, avoiding long paths in logs.
constexpr std::string_view BaseName(std::string_view path) noexcept {
  const auto pos = path.find_last_of('/');
  return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

/// Formats and outputs. Deliberately a function template rather than macro-inlined, to shrink macro expansion size.
template <typename... Args>
void LogFormatted(LogLevel level, std::string_view file, unsigned line,
                  std::format_string<Args...> fmt, Args&&... args) noexcept {
  // std::format may throw on out-of-memory; a logging failure must never take down the driver.
  try {
    EmitLogLine(level, file, line, std::format(fmt, std::forward<Args>(args)...));
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Text() is only a table lookup returning string_view; it does not allocate or throw -- it is safe inside this catch.
    EmitLogLine(LogLevel::kError, file, line, Text(Msg::kCommonLogFormatFailed));
  }
}

}  // namespace detail

/// Log macros. Compile-time level pruning first, then a runtime level check (branch-prediction friendly).
#define TETHERKITNEXT_LOG(level, ...)                                                          \
  do {                                                                                     \
    if constexpr (static_cast<int>(level) >= TETHERKITNEXT_COMPILED_MIN_LOG_LEVEL) {            \
      if (::tetherkitnext::detail::IsLogLevelEnabled(level)) [[unlikely]] {                     \
        ::tetherkitnext::detail::LogFormatted((level), ::tetherkitnext::detail::BaseName(__FILE__), \
                                          __LINE__, __VA_ARGS__);                           \
      }                                                                                     \
    }                                                                                       \
  } while (false)

#define TETHERKITNEXT_TRACE(...) TETHERKITNEXT_LOG(::tetherkitnext::LogLevel::kTrace, __VA_ARGS__)
#define TETHERKITNEXT_DEBUG(...) TETHERKITNEXT_LOG(::tetherkitnext::LogLevel::kDebug, __VA_ARGS__)
#define TETHERKITNEXT_INFO(...) TETHERKITNEXT_LOG(::tetherkitnext::LogLevel::kInfo, __VA_ARGS__)
#define TETHERKITNEXT_WARN(...) TETHERKITNEXT_LOG(::tetherkitnext::LogLevel::kWarn, __VA_ARGS__)
#define TETHERKITNEXT_ERROR(...) TETHERKITNEXT_LOG(::tetherkitnext::LogLevel::kError, __VA_ARGS__)

/// Emits a **translatable** log line. `id` is a `Msg` enum value, followed by arguments identical to std::format.
///
/// Why this group exists instead of writing `TETHERKITNEXT_INFO("{}", Tr(id, ...))` directly: only as macros can
/// Tr() stay **inside** the level check, so when the level is off both the lookup and the formatting are saved -- the
/// hand-written form above easily slips and puts Tr() outside the macro, computing it for nothing on every log line.
#define TETHERKITNEXT_LOG_TR(level, id, ...) \
  TETHERKITNEXT_LOG(level, "{}", ::tetherkitnext::Tr((id)__VA_OPT__(, ) __VA_ARGS__))

#define TETHERKITNEXT_TRACE_TR(id, ...) \
  TETHERKITNEXT_LOG_TR(::tetherkitnext::LogLevel::kTrace, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKITNEXT_DEBUG_TR(id, ...) \
  TETHERKITNEXT_LOG_TR(::tetherkitnext::LogLevel::kDebug, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKITNEXT_INFO_TR(id, ...) \
  TETHERKITNEXT_LOG_TR(::tetherkitnext::LogLevel::kInfo, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKITNEXT_WARN_TR(id, ...) \
  TETHERKITNEXT_LOG_TR(::tetherkitnext::LogLevel::kWarn, id __VA_OPT__(, ) __VA_ARGS__)
#define TETHERKITNEXT_ERROR_TR(id, ...) \
  TETHERKITNEXT_LOG_TR(::tetherkitnext::LogLevel::kError, id __VA_OPT__(, ) __VA_ARGS__)

}  // namespace tetherkitnext
