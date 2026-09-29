#include "tetherkitnext/common/i18n.h"

// The declaration of ContextSeparator() is in error.h (which does not want to drag in <format>),
// and the implementation is at the bottom of this file -- including it lets the compiler verify that the declaration and definition agree.
#include "tetherkitnext/common/error.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

namespace tetherkitnext {
namespace {

/// Number of messages. Both tables are fixed-length by it, guaranteeing that "added an enum but missed a translation" blows up at compile time.
constexpr std::size_t kMessageCount = static_cast<std::size_t>(Msg::kMessageCount);

/// The Chinese table.
constexpr std::array<std::string_view, kMessageCount> kChineseTable{
#define TETHERKITNEXT_MESSAGE(id, zh, en) zh,
#include "tetherkitnext/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERKITNEXT_MESSAGE
};

/// The English table.
constexpr std::array<std::string_view, kMessageCount> kEnglishTable{
#define TETHERKITNEXT_MESSAGE(id, zh, en) en,
#include "tetherkitnext/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERKITNEXT_MESSAGE
};

/// The current language. Process-wide single state, using an atomic rather than a mutex: every message needs to read it.
///
/// Default is English rather than Chinese: this table's call sites are all over the library's internals, and the host (CLI / GUI) overrides it at startup
/// immediately with SetLanguage. English is chosen as the default because "nobody ever set a language" usually means
/// the caller is a third-party binding, and English is more likely to be understood than Chinese.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<Language> g_language{Language::kEnglish};

/// ASCII lowercasing. Used only to compare language tags; locale need not be considered.
[[nodiscard]] char ToLowerAscii(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Whether `text` starts with `prefix` (ASCII case-insensitive).
[[nodiscard]] bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (ToLowerAscii(text[i]) != ToLowerAscii(prefix[i])) {
      return false;
    }
  }
  return true;
}

/// Reads an environment variable; returns an empty view when unset or an empty string.
[[nodiscard]] std::string_view ReadEnvironment(const char* name) noexcept {
  // getenv is flagged by concurrency-mt-unsafe because it is unsafe when concurrent with setenv. Here it
  // is read only once at process startup, before any worker thread is started, and the whole project never calls setenv.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return {};
  }
  return value;
}

}  // namespace

void SetLanguage(Language language) noexcept {
  g_language.store(language, std::memory_order_relaxed);
}

Language GetLanguage() noexcept {
  return g_language.load(std::memory_order_relaxed);
}

Language LanguageFromLocaleString(std::string_view locale) noexcept {
  return StartsWithIgnoreCase(locale, "zh") ? Language::kChinese : Language::kEnglish;
}

Language DetectLanguageFromEnvironment() noexcept {
  // The order follows POSIX: LC_ALL overrides LC_MESSAGES, and LC_MESSAGES overrides LANG.
  // TETHERKITNEXT_LANG comes first, so users can change only this program without touching locale settings.
  for (const char* name : {"TETHERKITNEXT_LANG", "LC_ALL", "LC_MESSAGES", "LANG"}) {
    const std::string_view value = ReadEnvironment(name);
    if (value.empty()) {
      continue;
    }
    // Once the first non-empty value is found it is decided and we do not look further -- otherwise LANG=zh_CN would be
    // interfered with by anything other than an empty value further down, and the semantics would not match POSIX either.
    return LanguageFromLocaleString(value);
  }
  return Language::kEnglish;
}

bool ParseLanguage(std::string_view text, Language* out_language) noexcept {
  if (out_language == nullptr) {
    return false;
  }
  if (StartsWithIgnoreCase(text, "zh") || StartsWithIgnoreCase(text, "chinese") ||
      text == "中文") {
    *out_language = Language::kChinese;
    return true;
  }
  if (StartsWithIgnoreCase(text, "en") || StartsWithIgnoreCase(text, "english")) {
    *out_language = Language::kEnglish;
    return true;
  }
  if (StartsWithIgnoreCase(text, "auto") || StartsWithIgnoreCase(text, "system")) {
    *out_language = DetectLanguageFromEnvironment();
    return true;
  }
  return false;
}

std::string_view LanguageTag(Language language) noexcept {
  return language == Language::kChinese ? "zh" : "en";
}

std::string_view TextIn(Language language, Msg id) noexcept {
  const auto index = static_cast<std::size_t>(id);
  if (index >= kMessageCount) {
    return {};
  }
  return language == Language::kChinese ? kChineseTable[index] : kEnglishTable[index];
}

std::string_view Text(Msg id) noexcept {
  return TextIn(GetLanguage(), id);
}

namespace detail {

std::string FormatMessage(Msg id, std::format_args args) noexcept {
  const std::string_view pattern = Text(id);
  try {
    return std::vformat(pattern, args);
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // This can only be that the placeholders in a translation do not match the call site (tests will catch it, but at runtime we would rather degrade than
    // throw). Fall back to the unsubstituted original text: incomplete information is still better than losing the whole message.
    return std::string{pattern};
  }
}

std::string_view ContextSeparator() noexcept {
  // Chinese uses a full-width colon (no spaces around it); English uses a half-width colon plus a space.
  return GetLanguage() == Language::kChinese ? "：" : ": ";
}

}  // namespace detail
}  // namespace tetherkitnext
