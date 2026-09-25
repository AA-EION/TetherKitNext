// 轻量分级日志。
//
// 设计约束：
//   1. **禁止出现在数据热路径上。** Trace/Debug 级别在 Release 构建下被
//      预处理器整段删掉（连参数求值都不会发生），所以在热路径写 TETHERKITNEXT_TRACE
//      是安全的；但 Info 及以上会真的做格式化与加锁写 stderr，热路径禁用。
//   2. 线程安全：多线程同时写 stderr 会交错，因此在一把互斥锁下整行输出。
//      日志不在热路径，锁竞争无所谓。
//   3. 不引入第三方日志库：只需要「时间 + 级别 + 线程名 + 位置 + 消息」，
//      std::format 已经够了。
#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "tetherkitnext/common/i18n.h"

namespace tetherkitnext {

enum class LogLevel : std::uint8_t {
  kTrace = 0,  ///< 逐帧级别的细节，Release 构建下编译期删除。
  kDebug = 1,  ///< 协议交互细节（每条 RNDIS 控制消息），Release 下编译期删除。
  kInfo = 2,   ///< 生命周期事件（设备连接、状态机迁移、统计报告）。
  kWarn = 3,   ///< 可恢复异常（USB stall、BPF 丢包、保活超时重试）。
  kError = 4,  ///< 不可恢复错误。
  kOff = 5,    ///< 全部关闭。
};

/// 编译期日志下限。低于此级别的日志调用被预处理器整段删除。
///
/// Release 构建（NDEBUG）下把 Trace/Debug 删掉，确保热路径上的
/// TETHERKITNEXT_TRACE 连参数都不求值。
#ifndef TETHERKITNEXT_COMPILED_MIN_LOG_LEVEL
#ifdef NDEBUG
#define TETHERKITNEXT_COMPILED_MIN_LOG_LEVEL 2  // kInfo
#else
#define TETHERKITNEXT_COMPILED_MIN_LOG_LEVEL 0  // kTrace
#endif
#endif

/// 运行期日志下限，默认 kInfo。
void SetLogLevel(LogLevel level) noexcept;

LogLevel GetLogLevel() noexcept;

/// 是否启用彩色输出。默认仅当 stderr 是 tty 时启用。
void SetLogColorEnabled(bool enabled) noexcept;

/// 给当前线程起个短名字，出现在日志行里，便于区分 usb-event / bpf-rx / bpf-tx。
/// 同时调用 pthread_setname_np，使其在 Instruments / lldb 里也可见。
void SetCurrentThreadName(std::string_view name) noexcept;

/// 日志汇：把已格式化好的日志行**额外**转交给宿主（GUI 需要在界面上显示日志，
/// 而日志本身只往 stderr 走）。stderr 输出不受影响，两者并行。
///
/// ★ 实现日志汇时的三条硬约束 ★
///   1. 会在**任意线程**上被调用（含 libusb 事件线程）；
///   2. 会在日志互斥锁**内部**被调用 —— 因此实现里**绝不能再打日志**
///      （std::mutex 不可重入，会当场死锁）；
///   3. 绝不能做阻塞 I/O —— 它会把所有正在打日志的线程一起拖住。
///
/// 满足这三条的唯一合理实现就是「拷进定长环形缓冲，由宿主轮询取走」。
using LogSink = void (*)(LogLevel level, std::string_view thread_name, std::string_view message,
                         void* user) noexcept;

/// 安装 / 卸载日志汇。传 nullptr 卸载。线程安全。
void SetLogSink(LogSink sink, void* user) noexcept;

namespace detail {

[[nodiscard]] bool IsLogLevelEnabled(LogLevel level) noexcept;

/// 输出一整行日志。`message` 已格式化完毕。
void EmitLogLine(LogLevel level, std::string_view file, unsigned line,
                 std::string_view message) noexcept;

/// 从 __FILE__ 里截出文件名，避免日志里出现长路径。
constexpr std::string_view BaseName(std::string_view path) noexcept {
  const auto pos = path.find_last_of('/');
  return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

/// 格式化并输出。故意做成函数模板而非宏内联，缩小宏展开体积。
template <typename... Args>
void LogFormatted(LogLevel level, std::string_view file, unsigned line,
                  std::format_string<Args...> fmt, Args&&... args) noexcept {
  // std::format 可能因内存不足抛异常；日志失败绝不应该拖垮驱动。
  try {
    EmitLogLine(level, file, line, std::format(fmt, std::forward<Args>(args)...));
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Text() 只是查表返回 string_view，不分配、不抛 —— 在这个 catch 里是安全的。
    EmitLogLine(LogLevel::kError, file, line, Text(Msg::kCommonLogFormatFailed));
  }
}

}  // namespace detail

/// 日志宏。先做编译期级别裁剪，再做运行期级别判断（分支预测友好）。
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

/// 打一条**可翻译**的日志。`id` 是 `Msg` 枚举值，其后的参数与 std::format 一致。
///
/// 为什么要有这一组而不是直接写 `TETHERKITNEXT_INFO("{}", Tr(id, ...))`：写成宏才能
/// 让 Tr() 留在级别判断的**内部**，级别没开时连查表带格式化一起省掉 —— 上面那种
/// 手写法很容易不小心把 Tr() 放到宏外面，每条日志白算一次。
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
