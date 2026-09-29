// Consistency check of the message table.
//
// * The sole reason this suite exists *
//
//   The format strings of Tr() come from a runtime table lookup, and the compiler **can no longer** check whether the placeholders match the arguments --
//   this is the price paid for replacing std::format with vformat. When placeholders do not match, vformat throws
//   std::format_error (we swallow it in FormatMessage, degrading to the unsubstituted original text),
//   so the error quietly turns into "one oddly shaped line in the log", which may go unnoticed for months.
//
//   So that safeguard must be made up here: compare, entry by entry, the argument indices and types referenced by the two languages.
//   Every message added to messages.def is automatically covered by this suite, with no manual registration.
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "tetherkitnext/common/error.h"
#include "tetherkitnext/common/i18n.h"

namespace {

using tetherkitnext::Language;
using tetherkitnext::Msg;

constexpr std::size_t kMessageCount = static_cast<std::size_t>(Msg::kMessageCount);

/// All replacement fields appearing in one format string.
struct ParsedFormat {
  /// Index -> the set of "presentation type" characters used by that index.
  ///
  /// The presentation type is the last letter of the format spec (`x` / `f` / `d`...), and it determines what type
  /// the argument must be. Width, alignment and fill are deliberately **not compared**: after a translation changes language, adjusting column width
  /// is entirely legitimate and does not affect type safety.
  std::map<std::size_t, std::string> types;
  /// Whether automatic numbering and manual numbering are mixed in the same string (std::format throws directly).
  bool mixed_indexing = false;
  /// Whether there is an unclosed `{`.
  bool malformed = false;
};

/// Extracts the presentation type character from a format spec. Returns an empty string when there is no spec or the spec ends with a non-letter.
[[nodiscard]] std::string PresentationType(std::string_view spec) {
  if (spec.empty()) {
    return {};
  }
  const char last = spec.back();
  return (std::isalpha(static_cast<unsigned char>(last)) != 0) ? std::string{last} : std::string{};
}

/// Parses a std::format format string and collects the replacement fields.
///
/// Implements only the syntax subset our messages actually use: `{}`, `{n}`, `{:spec}`, `{n:spec}`,
/// plus the `{{` / `}}` escapes. Nested dynamic widths (`{:{}}`) do not exist in this table, and their appearance is treated as malformed.
[[nodiscard]] ParsedFormat ParseFormat(std::string_view text) {
  ParsedFormat parsed;
  std::size_t auto_index = 0;
  bool saw_auto = false;
  bool saw_manual = false;

  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '}') {
      // A legal `}}` escape; skip the second one.
      if (i + 1 < text.size() && text[i + 1] == '}') {
        ++i;
      }
      continue;
    }
    if (text[i] != '{') {
      continue;
    }
    if (i + 1 < text.size() && text[i + 1] == '{') {
      ++i;  // `{{` escape
      continue;
    }

    const std::size_t close = text.find('}', i);
    if (close == std::string_view::npos) {
      parsed.malformed = true;
      break;
    }
    const std::string_view body = text.substr(i + 1, close - i - 1);
    i = close;

    // Split into the "index" and "spec" halves.
    const std::size_t colon = body.find(':');
    const std::string_view index_text = body.substr(0, colon);
    const std::string_view spec =
        colon == std::string_view::npos ? std::string_view{} : body.substr(colon + 1);
    if (spec.find('{') != std::string_view::npos) {
      parsed.malformed = true;  // dynamic width; should not appear in this table
      continue;
    }

    std::size_t index = 0;
    if (index_text.empty()) {
      saw_auto = true;
      index = auto_index++;
    } else {
      saw_manual = true;
      for (const char c : index_text) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
          parsed.malformed = true;
          break;
        }
        index = (index * 10) + static_cast<std::size_t>(c - '0');
      }
    }
    parsed.types[index] = PresentationType(spec);
  }

  parsed.mixed_indexing = saw_auto && saw_manual;
  return parsed;
}

/// A readable label for error reports. The message table does not store identifier names (the X-macro expands only the text), so it uses
/// the ordinal + the beginning of the English text, enough to locate which line of messages.def.
[[nodiscard]] std::string Label(std::size_t index) {
  const std::string_view english = tetherkitnext::TextIn(Language::kEnglish, static_cast<Msg>(index));
  return std::format("message #{} (\"{}\")", index, english.substr(0, 48));
}

}  // namespace

TEST_SUITE("common.i18n") {

TEST_CASE("语言切换与查表") {
  const Language original = tetherkitnext::GetLanguage();

  tetherkitnext::SetLanguage(Language::kChinese);
  CHECK(tetherkitnext::GetLanguage() == Language::kChinese);
  CHECK(tetherkitnext::Text(Msg::kCoreRunStateRunning) == "运行中");

  tetherkitnext::SetLanguage(Language::kEnglish);
  CHECK(tetherkitnext::GetLanguage() == Language::kEnglish);
  CHECK(tetherkitnext::Text(Msg::kCoreRunStateRunning) == "running");

  tetherkitnext::SetLanguage(original);
}

TEST_CASE("Tr 按当前语言渲染并替换参数") {
  const Language original = tetherkitnext::GetLanguage();

  tetherkitnext::SetLanguage(Language::kChinese);
  const std::string chinese = tetherkitnext::Tr(Msg::kNetBpfBindFailed, "feth1");
  CHECK(chinese.find("feth1") != std::string::npos);

  tetherkitnext::SetLanguage(Language::kEnglish);
  const std::string english = tetherkitnext::Tr(Msg::kNetBpfBindFailed, "feth1");
  CHECK(english.find("feth1") != std::string::npos);
  CHECK(english != chinese);

  tetherkitnext::SetLanguage(original);
}

TEST_CASE("每条文案两种语言都非空") {
  for (std::size_t i = 0; i < kMessageCount; ++i) {
    const auto id = static_cast<Msg>(i);
    CAPTURE(i);
    CHECK_FALSE(tetherkitnext::TextIn(Language::kChinese, id).empty());
    CHECK_FALSE(tetherkitnext::TextIn(Language::kEnglish, id).empty());
  }
}

TEST_CASE("越界的 Msg 返回空串而不是崩溃") {
  const auto out_of_range = static_cast<Msg>(kMessageCount);
  CHECK(tetherkitnext::Text(out_of_range).empty());
  CHECK(tetherkitnext::TextIn(Language::kChinese, out_of_range).empty());
}

// The core of this suite: the thing the compiler no longer checks is checked here entry by entry.
TEST_CASE("两种语言的占位符下标与类型一致") {
  for (std::size_t i = 0; i < kMessageCount; ++i) {
    const auto id = static_cast<Msg>(i);
    const ParsedFormat chinese = ParseFormat(tetherkitnext::TextIn(Language::kChinese, id));
    const ParsedFormat english = ParseFormat(tetherkitnext::TextIn(Language::kEnglish, id));

    CAPTURE(Label(i));
    CHECK_FALSE(chinese.malformed);
    CHECK_FALSE(english.malformed);
    // Mixing `{}` and `{0}` in one string makes std::format throw directly, and the compile time cannot stop it.
    CHECK_FALSE(chinese.mixed_indexing);
    CHECK_FALSE(english.mixed_indexing);

    // The number and indices of arguments must match exactly: one fewer renders an incomplete sentence, one more throws directly.
    CHECK(chinese.types.size() == english.types.size());
    for (const auto& [index, type] : chinese.types) {
      const auto found = english.types.find(index);
      REQUIRE(found != english.types.end());
      // Different type characters mean the two sides expect different argument types, so one side is bound to throw.
      CHECK(found->second == type);
    }

    // Indices must be consecutive 0..n-1: leaving a gap std::format can still render, but that means some
    // argument is ignored by both languages at once, which is almost always a mistake.
    std::size_t expected = 0;
    for (const auto& [index, type] : chinese.types) {
      CHECK(index == expected);
      ++expected;
    }
  }
}

TEST_CASE("ParseLanguage 认得常见写法") {
  Language language = Language::kEnglish;

  CHECK(tetherkitnext::ParseLanguage("zh", &language));
  CHECK(language == Language::kChinese);
  CHECK(tetherkitnext::ParseLanguage("zh-Hans", &language));
  CHECK(language == Language::kChinese);
  CHECK(tetherkitnext::ParseLanguage("zh_CN.UTF-8", &language));
  CHECK(language == Language::kChinese);
  CHECK(tetherkitnext::ParseLanguage("Chinese", &language));
  CHECK(language == Language::kChinese);

  CHECK(tetherkitnext::ParseLanguage("en", &language));
  CHECK(language == Language::kEnglish);
  CHECK(tetherkitnext::ParseLanguage("EN_US", &language));
  CHECK(language == Language::kEnglish);
  CHECK(tetherkitnext::ParseLanguage("english", &language));
  CHECK(language == Language::kEnglish);

  // auto uses environment inference; only require it to succeed and give one of the two languages.
  CHECK(tetherkitnext::ParseLanguage("auto", &language));

  CHECK_FALSE(tetherkitnext::ParseLanguage("klingon", &language));
  CHECK_FALSE(tetherkitnext::ParseLanguage("", &language));
  CHECK_FALSE(tetherkitnext::ParseLanguage("zh", nullptr));
}

TEST_CASE("区域设置串只按 zh 前缀判定") {
  CHECK(tetherkitnext::LanguageFromLocaleString("zh_CN.UTF-8") == Language::kChinese);
  CHECK(tetherkitnext::LanguageFromLocaleString("ZH-hant") == Language::kChinese);
  CHECK(tetherkitnext::LanguageFromLocaleString("en_US.UTF-8") == Language::kEnglish);
  CHECK(tetherkitnext::LanguageFromLocaleString("C") == Language::kEnglish);
  CHECK(tetherkitnext::LanguageFromLocaleString("") == Language::kEnglish);
  // "Not starting with zh" is English, even if zh appears elsewhere in the string.
  CHECK(tetherkitnext::LanguageFromLocaleString("en_zh") == Language::kEnglish);
}

TEST_CASE("语言标签") {
  CHECK(tetherkitnext::LanguageTag(Language::kChinese) == "zh");
  CHECK(tetherkitnext::LanguageTag(Language::kEnglish) == "en");
}

TEST_CASE("错误上下文的分隔符随语言变化") {
  const Language original = tetherkitnext::GetLanguage();

  // Context() returns a view, so the Error must first be put into a named variable before taking it,
  // otherwise the temporary is destroyed at the end of the statement and the view dangles on the spot.
  tetherkitnext::SetLanguage(Language::kChinese);
  const tetherkitnext::Error chinese = tetherkitnext::Error::Generic("inner").WithContext("outer");
  CHECK(chinese.Context().starts_with("outer："));

  tetherkitnext::SetLanguage(Language::kEnglish);
  const tetherkitnext::Error english = tetherkitnext::Error::Generic("inner").WithContext("outer");
  CHECK(english.Context().starts_with("outer: "));

  tetherkitnext::SetLanguage(original);
}

}  // TEST_SUITE
