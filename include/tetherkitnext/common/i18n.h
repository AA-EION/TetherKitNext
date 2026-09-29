// User-facing messages and runtime language switching.
//
// * Why not gettext *
//
//   gettext needs libintl, needs msgfmt to generate .mo files at build time, and needs a runtime lookup of
//   directories by path. This project needs only two languages and a few hundred fixed messages, and must serve the command line,
//   the shared library and the GUI at the same time (the GUI switches language through the C ABI). A table compiled into the binary is the simplest,
//   and avoids runtime failures such as "installed on another machine, .mo not found, so everything turns English".
//
// * How the message table is organized *
//
//   All messages are centralized in messages.def, one per line:
//
//     TETHERKITNEXT_MESSAGE(kCliUnknownOption, "未知选项 \"{}\"", "unknown option \"{}\"")
//
//   This file is expanded three times by the X-macro, generating the `Msg` enum, the Chinese table and the English table. All three
//   are generated from the same source, so it is **structurally impossible for one language to miss an entry**. The only remaining
//   place where things can go wrong is placeholders that do not match between the two languages, which tests/test_common_i18n.cc checks entry by entry.
//
// * How to use *
//
//   * Plain messages: `Tr(Msg::kCliUnknownOption, argument)` -- the arguments are exactly the same as std::format,
//     and it returns std::string.
//   * Logging: use macros like TETHERKITNEXT_INFO_TR (see logging.h), which keep the laziness of "do not evaluate
//     if the level is off".
//   * Errors: `Error::Generic(Tr(Msg::kFoo, x))`.
//
// * Forbidden on the hot path *
//
//   Tr() does a vformat and returns std::string, which necessarily allocates heap memory. Like error.h,
//   **it must never be called on the data hot path**.
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace tetherkitnext {

/// Currently supported UI languages.
///
/// Deliberately not an open-ended language tag string: each additional language needs an additional column of messages, and catching
/// "a missing entry" at compile time is worth more than falling back to some default language at runtime.
enum class Language : std::uint8_t {
  kEnglish = 0,
  kChinese = 1,
};

/// Message identifier. Values are generated from messages.def.
enum class Msg : std::uint16_t {
#define TETHERKITNEXT_MESSAGE(id, zh, en) id,
#include "tetherkitnext/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERKITNEXT_MESSAGE
  /// Sentinel: the number of messages. Only for table-length assertions and test iteration; not a real message.
  kMessageCount,
};

/// Sets the current language. Thread-safe; may be called at any time.
///
/// Process-wide single state: log and error strings are produced from multiple threads, and making it thread-local would only make the output of a single
/// session appear in two languages.
void SetLanguage(Language language) noexcept;

[[nodiscard]] Language GetLanguage() noexcept;

/// Infers the language from environment variables, checking in order `TETHERKITNEXT_LANG`, `LC_ALL`, `LC_MESSAGES`, `LANG`.
///
/// Takes the first non-empty value; if it starts with `zh` (case-insensitive) it is judged Chinese, and everything else is English;
/// if none of the four is set it is also English -- consistent with POSIX's default "C" locale.
///
/// WARNING: Whether `sudo` passes `LANG` through to the command depends on sudoers' `env_keep`, so the command line
/// also provides an explicit `--lang`; do not count on environment variables surviving under sudo.
[[nodiscard]] Language DetectLanguageFromEnvironment() noexcept;

/// Judges a locale string (`"zh_CN.UTF-8"`, `"en_US"`, `"C"`, ...) as a language.
///
/// There is only one criterion: starting with `zh` (case-insensitive) counts as Chinese, everything else is English. An empty string is likewise
/// English. DetectLanguageFromEnvironment feeds the environment variable's value to it --
/// it is split into a separate function so this criterion can be tested cleanly without touching environment variables.
[[nodiscard]] Language LanguageFromLocaleString(std::string_view locale) noexcept;

/// Parses the value of `--lang`. Accepts `zh` / `zh-Hans` / `zh_CN` / `chinese`,
/// `en` / `en-US` / `english`, and `auto` (infer from the environment).
///
/// @return true on successful parse, writing `out_language`; false if unrecognized, leaving
///         `out_language` unchanged.
[[nodiscard]] bool ParseLanguage(std::string_view text, Language* out_language) noexcept;

/// Short tag of the language (`"zh"` / `"en"`), used for echoing and logs.
[[nodiscard]] std::string_view LanguageTag(Language language) noexcept;

/// Gets the original text of a message in the **current language** (without argument substitution).
///
/// Returns an empty string instead of crashing when `id` is out of range -- a message table lookup failure must never bring down the driver.
[[nodiscard]] std::string_view Text(Msg id) noexcept;

/// Gets the original text of a message in the **specified language**. Tests use it to check placeholders language by language.
[[nodiscard]] std::string_view TextIn(Language language, Msg id) noexcept;

namespace detail {

/// Implementation of Tr(). Made a non-template function to avoid instantiating a copy of vformat at every call site.
///
/// Swallows std::format_error internally: when the placeholders in the message table do not match the call site (which can only happen if a
/// translation is wrong), it returns the unsubstituted original text instead of letting one log line take down the process.
[[nodiscard]] std::string FormatMessage(Msg id, std::format_args args) noexcept;

}  // namespace detail

/// Fetches a message in the current language and substitutes arguments. The argument semantics are exactly the same as `std::format`.
///
/// The only difference from `std::format` is that the format string comes from a runtime table lookup, so **the compiler cannot** check
/// whether the placeholders match the arguments. That safeguard is provided by tests/test_common_i18n.cc: it checks that
/// each message's placeholder set is identical in both languages, and does a trial run with real arguments.
template <typename... Args>
[[nodiscard]] std::string Tr(Msg id, const Args&... args) {
  return detail::FormatMessage(id, std::make_format_args(args...));
}

}  // namespace tetherkitnext
