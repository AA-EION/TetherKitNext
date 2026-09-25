// 面向用户的文案与运行期语言切换。
//
// ★ 为什么不用 gettext ★
//
//   gettext 要引入 libintl、要在构建期跑 msgfmt 生成 .mo、还要在运行期按路径
//   查目录。本项目只需要两种语言、几百条固定文案，且要能同时服务命令行、
//   共享库与 GUI（GUI 通过 C ABI 切换语言）。用一张编译进二进制的表最省事，
//   也不会出现「装到别的机器上找不到 .mo 于是全变英文」这种运行期故障。
//
// ★ 文案表怎么组织 ★
//
//   全部文案集中在 messages.def 里，一条一行：
//
//     TETHERKITNEXT_MESSAGE(kCliUnknownOption, "未知选项 \"{}\"", "unknown option \"{}\"")
//
//   这个文件被 X-macro 展开三次，分别生成 `Msg` 枚举、中文表与英文表。三者
//   由同一份源头生成，**结构上不可能出现某种语言漏了一条**。剩下唯一会出错的
//   地方是两种语言的占位符对不上，那由 tests/test_common_i18n.cc 逐条核对。
//
// ★ 怎么用 ★
//
//   * 普通文案：`Tr(Msg::kCliUnknownOption, argument)` —— 参数与 std::format
//     完全一致，返回 std::string。
//   * 日志：用 TETHERKITNEXT_INFO_TR 等宏（见 logging.h），它们保留了「级别没开就
//     不求值」的惰性。
//   * 错误：`Error::Generic(Tr(Msg::kFoo, x))`。
//
// ★ 热路径禁用 ★
//
//   Tr() 会做一次 vformat 并返回 std::string，必然分配堆内存。和 error.h 一样，
//   **数据热路径绝不允许调用**。
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace tetherkitnext {

/// 目前支持的界面语言。
///
/// 刻意不做成开放式的语言标签字符串：多一种语言就要多一列文案，编译期能查出
/// 「漏了一条」比运行期回落到某个默认语言更有价值。
enum class Language : std::uint8_t {
  kEnglish = 0,
  kChinese = 1,
};

/// 文案标识。取值由 messages.def 生成。
enum class Msg : std::uint16_t {
#define TETHERKITNEXT_MESSAGE(id, zh, en) id,
#include "tetherkitnext/common/messages.def"  // NOLINT(bugprone-suspicious-include)
#undef TETHERKITNEXT_MESSAGE
  /// 哨兵：文案条数。仅供表长断言与测试遍历使用，不是一条真实文案。
  kMessageCount,
};

/// 设置当前语言。线程安全，任何时候都可以调用。
///
/// 进程级的单一状态：日志与错误串会从多个线程产生，做成线程局部只会让同一次
/// 会话的输出出现两种语言。
void SetLanguage(Language language) noexcept;

[[nodiscard]] Language GetLanguage() noexcept;

/// 按环境变量推断语言，依次看 `TETHERKITNEXT_LANG`、`LC_ALL`、`LC_MESSAGES`、`LANG`。
///
/// 取第一个非空的值，以 `zh` 开头（大小写不敏感）时判定为中文，其余一律英文；
/// 四个都没设时也是英文 —— 这与 POSIX 的 "C" 缺省区域一致。
///
/// ⚠️ `sudo` 是否把 `LANG` 透传给命令取决于 sudoers 的 `env_keep`，因此命令行
/// 还提供 `--lang` 显式指定，不要指望环境变量在 sudo 下一定还在。
[[nodiscard]] Language DetectLanguageFromEnvironment() noexcept;

/// 把一个区域设置串（`"zh_CN.UTF-8"`、`"en_US"`、`"C"`……）判定成语言。
///
/// 判据只有一条：以 `zh` 开头（大小写不敏感）算中文，其余一概英文。空串同样
/// 算英文。DetectLanguageFromEnvironment 就是拿环境变量的值来喂它 ——
/// 拆成独立函数是为了能不碰环境变量就把这条判据测干净。
[[nodiscard]] Language LanguageFromLocaleString(std::string_view locale) noexcept;

/// 解析 `--lang` 的取值。接受 `zh` / `zh-Hans` / `zh_CN` / `chinese`、
/// `en` / `en-US` / `english`、以及 `auto`（按环境推断）。
///
/// @return 解析成功返回 true 并写入 `out_language`；无法识别时返回 false 且
///         不改动 `out_language`。
[[nodiscard]] bool ParseLanguage(std::string_view text, Language* out_language) noexcept;

/// 语言的短标签（`"zh"` / `"en"`），用于回显与日志。
[[nodiscard]] std::string_view LanguageTag(Language language) noexcept;

/// 取某条文案在**当前语言**下的原文（未做参数替换）。
///
/// `id` 越界时返回空串而不是崩溃 —— 文案查表失败绝不该拖垮驱动。
[[nodiscard]] std::string_view Text(Msg id) noexcept;

/// 取某条文案在**指定语言**下的原文。测试用它逐语言核对占位符。
[[nodiscard]] std::string_view TextIn(Language language, Msg id) noexcept;

namespace detail {

/// Tr() 的实现。做成非模板函数，避免每个调用点都实例化一份 vformat。
///
/// 内部吞掉 std::format_error：文案表里的占位符与调用点对不上时（只可能是
/// 翻译写错了），返回未替换的原文，而不是让一次日志把进程掀翻。
[[nodiscard]] std::string FormatMessage(Msg id, std::format_args args) noexcept;

}  // namespace detail

/// 按当前语言取文案并替换参数。参数语义与 `std::format` 完全一致。
///
/// 与 `std::format` 的唯一区别是格式串来自运行期查表，因此**编译器无法**检查
/// 占位符与参数是否匹配。这一层保障由 tests/test_common_i18n.cc 承担：它会核对
/// 每条文案两种语言的占位符集合一致，并用真实参数试跑一遍。
template <typename... Args>
[[nodiscard]] std::string Tr(Msg id, const Args&... args) {
  return detail::FormatMessage(id, std::make_format_args(args...));
}

}  // namespace tetherkitnext
