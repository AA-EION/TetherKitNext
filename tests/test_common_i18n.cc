// 文案表的一致性检查。
//
// ★ 这个 suite 存在的唯一理由 ★
//
//   Tr() 的格式串来自运行期查表，编译器**没法**再检查占位符与参数是否匹配 ——
//   这是把 std::format 换成 vformat 付出的代价。占位符对不上时 vformat 会抛
//   std::format_error（我们在 FormatMessage 里吞掉了，降级成未替换的原文），
//   于是错误会安静地变成「日志里有一行长得很奇怪」，可能几个月都没人发现。
//
//   所以那道保障必须在这里补回来：逐条比对两种语言引用的参数下标与类型。
//   messages.def 里每加一条文案，都会自动被本 suite 覆盖，不需要手动登记。
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

/// 一个格式串里出现的所有替换字段。
struct ParsedFormat {
  /// 下标 → 该下标用到的「表现类型」字符集合。
  ///
  /// 表现类型就是格式规格的最后一个字母（`x` / `f` / `d`……），它决定了实参
  /// 必须是什么类型。宽度、对齐、填充刻意**不比**：译文换了语言之后想调列宽
  /// 是完全正当的，那不影响类型安全。
  std::map<std::size_t, std::string> types;
  /// 是否在同一个串里混用了自动编号与手工编号（std::format 会直接抛）。
  bool mixed_indexing = false;
  /// 是否有未闭合的 `{`。
  bool malformed = false;
};

/// 从格式规格里取出表现类型字符。没有规格、或规格以非字母结尾时返回空串。
[[nodiscard]] std::string PresentationType(std::string_view spec) {
  if (spec.empty()) {
    return {};
  }
  const char last = spec.back();
  return (std::isalpha(static_cast<unsigned char>(last)) != 0) ? std::string{last} : std::string{};
}

/// 解析一个 std::format 格式串，收集替换字段。
///
/// 只实现我们文案里真正用到的语法子集：`{}`、`{n}`、`{:spec}`、`{n:spec}`，
/// 外加 `{{` / `}}` 转义。嵌套的动态宽度（`{:{}}`）本表里没有，出现即视为畸形。
[[nodiscard]] ParsedFormat ParseFormat(std::string_view text) {
  ParsedFormat parsed;
  std::size_t auto_index = 0;
  bool saw_auto = false;
  bool saw_manual = false;

  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '}') {
      // 合法的 `}}` 转义，跳过第二个。
      if (i + 1 < text.size() && text[i + 1] == '}') {
        ++i;
      }
      continue;
    }
    if (text[i] != '{') {
      continue;
    }
    if (i + 1 < text.size() && text[i + 1] == '{') {
      ++i;  // `{{` 转义
      continue;
    }

    const std::size_t close = text.find('}', i);
    if (close == std::string_view::npos) {
      parsed.malformed = true;
      break;
    }
    const std::string_view body = text.substr(i + 1, close - i - 1);
    i = close;

    // 拆成「下标」与「规格」两半。
    const std::size_t colon = body.find(':');
    const std::string_view index_text = body.substr(0, colon);
    const std::string_view spec =
        colon == std::string_view::npos ? std::string_view{} : body.substr(colon + 1);
    if (spec.find('{') != std::string_view::npos) {
      parsed.malformed = true;  // 动态宽度，本表不该出现
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

/// 给报错用的可读标签。文案表里没存标识名（X-macro 只展开出文字），所以用
/// 序号 + 英文原文的开头，足够定位到 messages.def 的哪一行。
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

// 本 suite 的核心：编译器不再检查的那件事，在这里逐条检查。
TEST_CASE("两种语言的占位符下标与类型一致") {
  for (std::size_t i = 0; i < kMessageCount; ++i) {
    const auto id = static_cast<Msg>(i);
    const ParsedFormat chinese = ParseFormat(tetherkitnext::TextIn(Language::kChinese, id));
    const ParsedFormat english = ParseFormat(tetherkitnext::TextIn(Language::kEnglish, id));

    CAPTURE(Label(i));
    CHECK_FALSE(chinese.malformed);
    CHECK_FALSE(english.malformed);
    // 同一个串里混用 `{}` 与 `{0}` 会让 std::format 直接抛，编译期又拦不住。
    CHECK_FALSE(chinese.mixed_indexing);
    CHECK_FALSE(english.mixed_indexing);

    // 参数个数与下标必须完全一致：少一个就会渲染出残缺的句子，多一个直接抛。
    CHECK(chinese.types.size() == english.types.size());
    for (const auto& [index, type] : chinese.types) {
      const auto found = english.types.find(index);
      REQUIRE(found != english.types.end());
      // 类型字符不同意味着两边期待的实参类型不同，必然有一边会抛。
      CHECK(found->second == type);
    }

    // 下标必须是连续的 0..n-1：留空档时 std::format 照样能渲染，但那说明有个
    // 实参被两种语言同时忽略了，几乎总是写错。
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

  // auto 走环境推断，只要求它成功并给出两种语言之一。
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
  // 「不以 zh 开头」就是英文，哪怕串里别处有 zh。
  CHECK(tetherkitnext::LanguageFromLocaleString("en_zh") == Language::kEnglish);
}

TEST_CASE("语言标签") {
  CHECK(tetherkitnext::LanguageTag(Language::kChinese) == "zh");
  CHECK(tetherkitnext::LanguageTag(Language::kEnglish) == "en");
}

TEST_CASE("错误上下文的分隔符随语言变化") {
  const Language original = tetherkitnext::GetLanguage();

  // Context() 返回的是 view，必须先把 Error 落到具名变量上再取，
  // 否则临时对象在语句末尾析构，view 当场悬垂。
  tetherkitnext::SetLanguage(Language::kChinese);
  const tetherkitnext::Error chinese = tetherkitnext::Error::Generic("inner").WithContext("outer");
  CHECK(chinese.Context().starts_with("outer："));

  tetherkitnext::SetLanguage(Language::kEnglish);
  const tetherkitnext::Error english = tetherkitnext::Error::Generic("inner").WithContext("outer");
  CHECK(english.Context().starts_with("outer: "));

  tetherkitnext::SetLanguage(original);
}

}  // TEST_SUITE
