// TetherKitNext command-line entry point.
//
// Responsibilities are deliberately kept thin: parse arguments -> assemble RuntimeConfig -> hand to core::Runtime ->
// handle signals. All substantive logic is in the library, so that it can be covered by unit tests.
#include <signal.h>  // NOLINT(modernize-deprecated-headers) -- sigaction is only in this header
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

/// Global shutdown flag.
///
/// Process-level signal state is inherently global and unavoidable; after using a lock-free atomic to guarantee safety, it is not wrapped further.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
/// **In a signal handler only volatile sig_atomic_t or lock-free atomics may be touched**; locking,
/// allocating memory or logging must never be done -- none of those are async-signal-safe. Here only one atomic is written,
/// and the real shutdown is performed by the main thread after it sees it.
std::atomic<bool> g_stop_requested{false};

extern "C" void HandleSignal(int /*signal_number*/) {
  g_stop_requested.store(true, std::memory_order_release);
}

/// Installs the signal handlers.
///
/// sigaction is used rather than signal(): the latter's semantics are inconsistent across Unixes,
/// and by default it resets to SIG_DFL while the handler is executing.
[[nodiscard]] Status InstallSignalHandlers() {
  struct sigaction action{};
  action.sa_handler = &HandleSignal;
  // Note: on macOS sigemptyset is a **macro** (#define sigemptyset(set) (*(set)=0,0)),
  // so it cannot be written as ::sigemptyset -- a macro name cannot be scope-qualified.
  sigemptyset(&action.sa_mask);
  // Deliberately **not setting** SA_RESTART: we want signals to interrupt blocking system calls (such as BPF's
  // read()), giving threads a chance to see the shutdown flag.
  action.sa_flags = 0;

  for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
    if (::sigaction(signal_number, &action, nullptr) != 0) {
      return std::unexpected(
          Error::FromErrno(0, Tr(Msg::kCliInstallSignalHandlerFailed, signal_number)));
    }
  }

  // Ignore SIGPIPE: writing to a closed BPF descriptor triggers it, and the default behavior is to terminate the process.
  struct sigaction ignore{};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kCliIgnoreSigpipeFailed)));
  }
  return tetherkitnext::Ok();
}

// =============================================================================
// Command-line parsing
// =============================================================================

/// Writes a message without arguments to stdout as-is.
///
/// fwrite is used instead of fputs: the string_view returned by `Text()` points to a literal in the table,
/// which in practice does carry a NUL, but string_view's contract does not include that; do not form bad habits.
void PrintText(Msg id) {
  const std::string_view text = Text(id);
  std::fwrite(text.data(), 1, text.size(), stdout);
}

void PrintUsage() {
  PrintText(Msg::kCliUsage);
}

/// Picks out `--lang` and makes it take effect **before** the arguments are formally parsed.
///
/// Why scan separately: the help text, and the errors produced during parsing itself, must all use the language the user wants.
/// By the time ParseArguments reaches `--lang` in order, the error messages of the earlier options
/// have already been emitted in the old language.
///
/// Only the `--lang <value>` form is recognized (none of the project's options support `--opt=value`, and no exception is made
/// here, otherwise ParseArguments would treat `--lang=en` as an unknown option, which is more confusing).
/// When the value is illegal no error is reported here, leaving it to ParseArguments to report -- by then the error itself is already in
/// the language the user wants.
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

/// Parses an unsigned integer argument.
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

/// Parse result.
struct ParsedArguments {
  tetherkitnext::core::RuntimeConfig config;
  bool show_help = false;
  bool show_version = false;
  bool list_devices = false;
};

[[nodiscard]] Result<ParsedArguments> ParseArguments(int argc, char** argv) {
  ParsedArguments parsed;

  // Take the value of an option that needs a value.
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
      // The value has already taken effect before parsing began via ApplyLanguageOption (so even this round's error messages
      // are in the target language). Here we only consume it and re-check the spelling.
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

/// `--list`: lists the recognized RNDIS devices. **No root needed**.
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
  // The language must be settled before **any** output. Infer from the environment first, then let an explicit --lang override --
  // so that even the errors reported by argument parsing itself are in the language the user wants.
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

  // A signal handler can only set an atomic (the only async-signal-safe approach), so someone needs to relay it
  // to the runtime. A lightweight watcher thread is used rather than having Runtime read a process-level global directly --
  // this way Runtime depends on no global state, which makes testing easier.
  std::thread signal_watcher([&runtime] {
    while (!g_stop_requested.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TETHERKITNEXT_INFO_TR(Msg::kCliStopSignalReceived);
    (*runtime)->RequestStop();
  });

  // The control loop runs on Runtime's own thread (see the threading model explanation in core/runtime.h),
  // and the main thread only needs to park here waiting for it to finish.
  (*runtime)->WaitUntilStopped();

  // The control loop may have exited because of an internal fatal error (rather than receiving a signal),
  // in which case the watcher thread must also be made to finish.
  g_stop_requested.store(true, std::memory_order_release);
  signal_watcher.join();

  (*runtime)->Stop();

  // The startup sequence runs asynchronously, and failure does not show up in the return value of Start() -- take it from the snapshot.
  // The runtime has already logged the error content; here we only carry the exit code out, so scripts can tell.
  const auto snapshot = (*runtime)->Snapshot();
  return snapshot.fatal_message.empty() ? 0 : 1;
}
