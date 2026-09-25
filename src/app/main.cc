// TetherKitNext 命令行入口。
//
// 职责刻意保持很薄：解析参数 → 装配 RuntimeConfig → 交给 core::Runtime →
// 处理信号。所有实质逻辑都在库里，这样才能被单元测试覆盖。
#include <signal.h>  // NOLINT(modernize-deprecated-headers) —— sigaction 只在此头文件里
#include <unistd.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/core/runtime.h"
#include "tetherkitnext/version.h"

namespace {

using tetherkitnext::Error;
using tetherkitnext::Language;
using tetherkitnext::LogLevel;
using tetherkitnext::Msg;
using tetherkitnext::Result;
using tetherkitnext::Status;
using tetherkitnext::Text;
using tetherkitnext::Tr;

/// 全局停机标志。
///
/// 进程级的信号状态天然是全局的，无法避免；用无锁原子保证安全后不再包装。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
/// **信号处理器里只能碰 volatile sig_atomic_t 或无锁原子**，绝不能加锁、
/// 分配内存或打日志 —— 那些都不是异步信号安全的。这里只写一个原子，
/// 真正的停机由主线程在看到它之后执行。
std::atomic<bool> g_stop_requested{false};

extern "C" void HandleSignal(int /*signal_number*/) {
  g_stop_requested.store(true, std::memory_order_release);
}

/// 安装信号处理器。
///
/// 用 sigaction 而非 signal()：后者的语义在各 Unix 上不一致，
/// 而且默认会在处理器执行期间重置为 SIG_DFL。
[[nodiscard]] Status InstallSignalHandlers() {
  struct sigaction action{};
  action.sa_handler = &HandleSignal;
  // 注意：macOS 上 sigemptyset 是**宏**（#define sigemptyset(set) (*(set)=0,0)），
  // 不能写成 ::sigemptyset —— 宏名无法被作用域限定。
  sigemptyset(&action.sa_mask);
  // 刻意**不设** SA_RESTART：我们希望信号能打断阻塞的系统调用（例如 BPF 的
  // read()），让线程有机会看到停机标志。
  action.sa_flags = 0;

  for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
    if (::sigaction(signal_number, &action, nullptr) != 0) {
      return std::unexpected(
          Error::FromErrno(0, Tr(Msg::kCliInstallSignalHandlerFailed, signal_number)));
    }
  }

  // 忽略 SIGPIPE：往已关闭的 BPF 描述符写会触发它，默认行为是终止进程。
  struct sigaction ignore{};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kCliIgnoreSigpipeFailed)));
  }
  return tetherkitnext::Ok();
}

// =============================================================================
// 命令行解析
// =============================================================================

/// 把一条不带参数的文案原样写到 stdout。
///
/// 用 fwrite 而不是 fputs：`Text()` 返回的 string_view 指向表里的字面量，
/// 虽然实际上带 NUL，但 string_view 的契约里没有这一条，别养成坏习惯。
void PrintText(Msg id) {
  const std::string_view text = Text(id);
  std::fwrite(text.data(), 1, text.size(), stdout);
}

void PrintUsage() {
  PrintText(Msg::kCliUsage);
}

/// 在正式解析参数**之前**先把 `--lang` 挑出来生效。
///
/// 为什么要单独扫一遍：帮助文本、以及解析过程中自己产生的报错，都得用用户要的
/// 那种语言。等 ParseArguments 顺序走到 `--lang` 时，前面几个选项的错误消息
/// 已经用旧语言发出去了。
///
/// 只认 `--lang <值>` 这一种写法（全项目的选项都不支持 `--opt=值`，不在这里
/// 破例，否则 ParseArguments 会把 `--lang=en` 当成未知选项，反而更费解）。
/// 值非法时这里不报错，留给 ParseArguments 去报 —— 那时报错本身已经是
/// 用户要的语言了。
void ApplyLanguageOption(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view{argv[i]} != "--lang") {
      continue;
    }
    if (Language language{}; tetherkitnext::ParseLanguage(argv[i + 1], &language)) {
      tetherkitnext::SetLanguage(language);
    }
    return;
  }
}

/// 解析一个无符号整数参数。
[[nodiscard]] Result<std::uint32_t> ParseUint(std::string_view text, int base = 10) {
  std::uint32_t value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, error_code] = std::from_chars(begin, end, value, base);
  if (error_code != std::errc{} || ptr != end) {
    return std::unexpected(Error::Generic(Tr(Msg::kCliParseNumberFailed, text)));
  }
  return value;
}

[[nodiscard]] Result<LogLevel> ParseLogLevel(std::string_view text) {
  if (text == "trace") {
    return LogLevel::kTrace;
  }
  if (text == "debug") {
    return LogLevel::kDebug;
  }
  if (text == "info") {
    return LogLevel::kInfo;
  }
  if (text == "warn") {
    return LogLevel::kWarn;
  }
  if (text == "error") {
    return LogLevel::kError;
  }
  if (text == "off") {
    return LogLevel::kOff;
  }
  return std::unexpected(Error::Generic(Tr(Msg::kCliUnknownLogLevel, text)));
}

/// 解析结果。
struct ParsedArguments {
  tetherkitnext::core::RuntimeConfig config;
  bool show_help = false;
  bool show_version = false;
  bool list_devices = false;
};

[[nodiscard]] Result<ParsedArguments> ParseArguments(int argc, char** argv) {
  ParsedArguments parsed;

  // 取一个需要值的选项的值。
  const auto take_value = [argc, argv](int& index,
                                       std::string_view option) -> Result<std::string_view> {
    if (index + 1 >= argc) {
      return std::unexpected(Error::Generic(Tr(Msg::kCliMissingOptionValue, option)));
    }
    ++index;
    return std::string_view{argv[index]};
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};

    if (argument == "-h" || argument == "--help") {
      parsed.show_help = true;
      return parsed;
    }
    if (argument == "-V" || argument == "--version") {
      parsed.show_version = true;
      return parsed;
    }
    if (argument == "--list") {
      parsed.list_devices = true;
      continue;
    }
    if (argument == "--keep-feth-mac") {
      parsed.config.adopt_device_mac = false;
      continue;
    }
    if (argument == "--no-color") {
      tetherkitnext::SetLogColorEnabled(false);
      continue;
    }

    if (argument == "--vid") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto value, ParseUint(text, 16));
      parsed.config.device_filter.vendor_id = static_cast<std::uint16_t>(value);
      continue;
    }
    if (argument == "--pid") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto value, ParseUint(text, 16));
      parsed.config.device_filter.product_id = static_cast<std::uint16_t>(value);
      continue;
    }
    if (argument == "--mtu") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(parsed.config.mtu, ParseUint(text));
      continue;
    }
    if (argument == "--rx-transfers") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(parsed.config.data_channel.rx_transfer_count, ParseUint(text));
      continue;
    }
    if (argument == "--tx-transfers") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(parsed.config.data_channel.tx_transfer_count, ParseUint(text));
      continue;
    }
    if (argument == "--rx-transfer-kb") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto kilobytes, ParseUint(text));
      parsed.config.data_channel.rx_transfer_bytes = kilobytes * 1024;
      continue;
    }
    if (argument == "--max-transfer-kb") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto kilobytes, ParseUint(text));
      parsed.config.rndis.host_max_transfer_size = kilobytes * 1024;
      continue;
    }
    if (argument == "--max-tx-packets") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(parsed.config.rndis.max_tx_packets_per_message, ParseUint(text));
      continue;
    }
    if (argument == "--bpf-buffer-kb") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto kilobytes, ParseUint(text));
      parsed.config.bpf.kernel_buffer_bytes = kilobytes * 1024;
      continue;
    }
    if (argument == "--stats") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(parsed.config.stats_interval_millis, ParseUint(text));
      continue;
    }
    if (argument == "--log") {
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto level, ParseLogLevel(text));
      tetherkitnext::SetLogLevel(level);
      continue;
    }
    if (argument == "--lang") {
      // 值已经由 ApplyLanguageOption 在解析开始前生效了（那样连本轮的报错
      // 都是目标语言）。这里只把它消费掉并复核一次拼写。
      TETHERKITNEXT_ASSIGN_OR_RETURN(const auto text, take_value(i, argument));
      Language ignored{};
      if (!tetherkitnext::ParseLanguage(text, &ignored)) {
        return std::unexpected(Error::Generic(Tr(Msg::kCliUnknownLanguage, text)));
      }
      continue;
    }

    return std::unexpected(Error::Generic(Tr(Msg::kCliUnknownOption, argument)));
  }
  return parsed;
}

/// `--list`：列出识别到的 RNDIS 设备。**不需要 root**。
[[nodiscard]] Status ListDevices(const tetherkitnext::usb::DeviceFilter& filter) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto context, tetherkitnext::usb::Context::Create());
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto candidates,
                             tetherkitnext::usb::FindRndisDevices(*context, filter));

  if (candidates.empty()) {
    PrintText(Msg::kCliNoDevices);
    PrintText(Msg::kCliNoDevicesHint);
    return tetherkitnext::Ok();
  }

  PrintText(Msg::kCliDevicesHeader);
  for (const auto& candidate : candidates) {
    const std::string line =
        Tr(Msg::kCliDeviceLine, candidate.Describe(), candidate.control_interface,
           candidate.data_interface, candidate.signature.interface_class,
           candidate.signature.interface_subclass, candidate.signature.interface_protocol,
           candidate.used_android_quirk ? Text(Msg::kCliDeviceAndroidQuirk) : std::string_view{});
    std::fputs(line.c_str(), stdout);
  }
  return tetherkitnext::Ok();
}

}  // namespace

int main(int argc, char** argv) {
  // 语言要在**任何**输出之前定下来。先按环境推断，再让显式的 --lang 覆盖 ——
  // 这样连参数解析自己报的错也是用户要的那种语言。
  tetherkitnext::SetLanguage(tetherkitnext::DetectLanguageFromEnvironment());
  ApplyLanguageOption(argc, argv);

  auto parsed = ParseArguments(argc, argv);
  if (!parsed) {
    const std::string message = Tr(Msg::kCliArgumentError, parsed.error().ToString());
    std::fputs(message.c_str(), stderr);
    return 2;
  }

  if (parsed->show_help) {
    PrintUsage();
    return 0;
  }
  if (parsed->show_version) {
    const std::string line = std::format("{}\n{}\n", tetherkitnext::GetVersionString(),
                                         tetherkitnext::GetBuildDescription());
    std::fputs(line.c_str(), stdout);
    return 0;
  }

  if (parsed->list_devices) {
    if (const auto status = ListDevices(parsed->config.device_filter); !status) {
      const std::string message = Tr(Msg::kCliEnumerateFailed, status.error().ToString());
      std::fputs(message.c_str(), stderr);
      return 1;
    }
    return 0;
  }

  if (const auto status = InstallSignalHandlers(); !status) {
    TETHERKITNEXT_ERROR("{}", status.error().ToString());
    return 1;
  }

  TETHERKITNEXT_INFO("{}", tetherkitnext::GetVersionString());

  auto runtime = tetherkitnext::core::Runtime::Create(parsed->config);
  if (!runtime) {
    TETHERKITNEXT_ERROR("{}", runtime.error().ToString());
    return 1;
  }

  if (const auto status = (*runtime)->Start(); !status) {
    TETHERKITNEXT_ERROR_TR(Msg::kCliStartFailed, status.error().ToString());
    return 1;
  }

  // 信号处理器里只能置一个原子（异步信号安全的唯一做法），所以需要有人把它
  // 转达给运行时。用一个轻量的守望线程而不是让 Runtime 直接读进程级全局变量 ——
  // 这样 Runtime 不依赖任何全局状态，便于测试。
  std::thread signal_watcher([&runtime] {
    while (!g_stop_requested.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TETHERKITNEXT_INFO_TR(Msg::kCliStopSignalReceived);
    (*runtime)->RequestStop();
  });

  // 控制循环跑在 Runtime 自己的线程上（见 core/runtime.h 的线程模型说明），
  // 主线程只需要在这里挂起等它结束。
  (*runtime)->WaitUntilStopped();

  // 控制循环可能是因为内部致命错误退出的（而不是收到信号），
  // 这时也要让守望线程结束。
  g_stop_requested.store(true, std::memory_order_release);
  signal_watcher.join();

  (*runtime)->Stop();

  // 启动序列是异步跑的，失败不会体现在 Start() 的返回值里 —— 从快照里取。
  // 错误内容运行时已经打过日志，这里只负责把退出码带出去，好让脚本能判断。
  const auto snapshot = (*runtime)->Snapshot();
  return snapshot.fatal_message.empty() ? 0 : 1;
}
