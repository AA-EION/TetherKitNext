#include "tetherkitnext/common/i18n.h"

// ContextSeparator() 的声明在 error.h 里（那边不想拖上 <format>），
// 实现在本文件底部 —— 包含它才能让编译器核对声明与定义一致。
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

/// 文案条数。两张表都按它定长，保证「加了枚举却漏了译文」在编译期就炸。
constexpr std::size_t kMessageCount = static_cast<std::size_t>(Msg::kMessageCount);

/// 中文表。
constexpr std::array<std::string_view, kMessageCount> kChineseTable{
#define TETHERKITNEXT_MESSAGE(id, zh, en) zh,
#include "tetherkitnext/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERKITNEXT_MESSAGE
};

/// 英文表。
constexpr std::array<std::string_view, kMessageCount> kEnglishTable{
#define TETHERKITNEXT_MESSAGE(id, zh, en) en,
#include "tetherkitnext/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERKITNEXT_MESSAGE
};

/// 当前语言。进程级单一状态，用原子而非互斥：每条文案都要读它。
///
/// 默认英文而不是中文：本表的调用点遍布库内部，宿主（命令行 / GUI）会在启动时
/// 立刻用 SetLanguage 覆盖掉。选英文作缺省是因为「没人设置过语言」通常意味着
/// 调用方是第三方绑定，英文比中文更可能被看懂。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<Language> g_language{Language::kEnglish};

/// ASCII 小写化。只用来比较语言标签，不需要考虑区域设置。
[[nodiscard]] char ToLowerAscii(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// `text` 是否以 `prefix` 开头（ASCII 大小写不敏感）。
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

/// 读一个环境变量，未设置或为空串时返回空 view。
[[nodiscard]] std::string_view ReadEnvironment(const char* name) noexcept {
  // getenv 被 concurrency-mt-unsafe 标记，因为它与 setenv 并发时不安全。这里
  // 只在进程启动、还没起任何工作线程时读一次，且全项目从不调用 setenv。
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
  // 顺序遵循 POSIX：LC_ALL 压过 LC_MESSAGES，LC_MESSAGES 压过 LANG。
  // TETHERKITNEXT_LANG 排在最前，好让用户在不动区域设置的前提下只改本程序。
  for (const char* name : {"TETHERKITNEXT_LANG", "LC_ALL", "LC_MESSAGES", "LANG"}) {
    const std::string_view value = ReadEnvironment(name);
    if (value.empty()) {
      continue;
    }
    // 取到第一个非空值就定了，不再往后看 —— 否则 LANG=zh_CN 会被更靠后的
    // 空值以外的任何东西干扰，语义也和 POSIX 不符。
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
    // 只可能是译文里的占位符与调用点对不上（测试会拦住，但运行期宁可降级也
    // 不能抛）。退回未替换的原文：信息不全，总好过丢掉整条消息。
    return std::string{pattern};
  }
}

std::string_view ContextSeparator() noexcept {
  // 中文用全角冒号（前后不留空格），英文用半角冒号加空格。
  return GetLanguage() == Language::kChinese ? "：" : ": ";
}

}  // namespace detail
}  // namespace tetherkitnext
