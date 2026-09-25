import XCTest

@testable import TetherKitNextIPC

/// 文案表的一致性检查。
///
/// ★ 这些用例存在的唯一理由 ★
///
///   `L(...)` 走 `String(format:)`，格式串来自运行期查表 —— 编译器管不到它。
///   两种语言的占位符对不上时，`String(format:)` **不会报错**：它会安静地按
///   自己读到的类型去解释一个根本不是那个类型的参数。`%@` 撞上一个整数就是
///   把整数当指针解引用，直接崩；`%ld` 撞上字符串则打出一个地址。两种都是
///   「翻译写错 → 运行期炸在用户机器上」，而且中文版好好的、英文版才炸。
///
///   所以这道保障必须在这里补回来。`L10nKey` 是 CaseIterable，新加的文案
///   自动被覆盖，不需要手动登记。
final class LocalizationTests: XCTestCase {

    /// 一个替换字段：位置（显式 `%1$` 的编号，隐式则按出现顺序）+ 转换类型。
    private struct Placeholder: Equatable, CustomStringConvertible {
        let position: Int
        let conversion: String

        var description: String { "#\(position):%\(conversion)" }
    }

    /// 解析 printf 风格的格式串。
    ///
    /// 只覆盖本表真正用到的子集：`%@`、`%ld`、`%lx`、`%f` 及其带宽度/精度/位置
    /// 的形式，外加 `%%` 转义。
    private func placeholders(in text: String) -> [Placeholder] {
        var result: [Placeholder] = []
        var automatic = 0
        let characters = Array(text)
        var index = 0

        while index < characters.count {
            guard characters[index] == "%" else {
                index += 1
                continue
            }
            index += 1
            guard index < characters.count else { break }
            if characters[index] == "%" {  // `%%` 转义
                index += 1
                continue
            }

            // 显式位置：`3$`
            var explicit: Int?
            var digits = ""
            var lookahead = index
            while lookahead < characters.count, characters[lookahead].isNumber {
                digits.append(characters[lookahead])
                lookahead += 1
            }
            if lookahead < characters.count, characters[lookahead] == "$", !digits.isEmpty {
                explicit = Int(digits)
                index = lookahead + 1
            }

            // 标志、宽度、精度 —— 一概跳过，它们不影响实参类型。
            while index < characters.count,
                  "0123456789.-+ #'".contains(characters[index]) {
                index += 1
            }
            // 长度修饰符（l / ll / h / z）属于类型的一部分，要留下。
            var conversion = ""
            while index < characters.count, "lhzqjt".contains(characters[index]) {
                conversion.append(characters[index])
                index += 1
            }
            guard index < characters.count else { break }
            conversion.append(characters[index])
            index += 1

            automatic += 1
            result.append(Placeholder(position: explicit ?? automatic, conversion: conversion))
        }
        return result.sorted { $0.position < $1.position }
    }

    func testEveryKeyHasBothLanguages() {
        for key in L10nKey.allCases {
            let (chinese, english) = key.localizations
            XCTAssertFalse(chinese.isEmpty, "\(key.rawValue) 缺中文")
            XCTAssertFalse(english.isEmpty, "\(key.rawValue) 缺英文")
        }
    }

    /// 本文件的核心用例。
    func testPlaceholdersMatchAcrossLanguages() {
        for key in L10nKey.allCases {
            let (chinese, english) = key.localizations
            let chinesePlaceholders = placeholders(in: chinese)
            let englishPlaceholders = placeholders(in: english)

            XCTAssertEqual(
                chinesePlaceholders, englishPlaceholders,
                """
                \(key.rawValue) 的占位符两种语言对不上：
                  中文 \(chinesePlaceholders) — \(chinese)
                  英文 \(englishPlaceholders) — \(english)
                """)

            // 位置必须是连续的 1...n。留空档时 String(format:) 照样能渲染，
            // 但那说明有个实参被两种语言同时忽略了，几乎总是写错。
            for (offset, placeholder) in chinesePlaceholders.enumerated() {
                XCTAssertEqual(placeholder.position, offset + 1,
                               "\(key.rawValue) 的占位符编号不连续：\(chinesePlaceholders)")
            }
        }
    }

    /// 整数一律 `%ld`：`%d` 只取 64 位实参的低 32 位，Swift 侧传的是 `Int`。
    func testIntegerPlaceholdersUseLongModifier() {
        for key in L10nKey.allCases {
            let (chinese, english) = key.localizations
            for text in [chinese, english] {
                for placeholder in placeholders(in: text) where
                    ["d", "i", "u", "x", "X"].contains(placeholder.conversion) {
                    XCTFail("""
                        \(key.rawValue) 用了 %\(placeholder.conversion)，应当是 \
                        %l\(placeholder.conversion) —— 调用点传的是 Int（64 位）。
                        """)
                }
            }
        }
    }

    func testLanguageResolution() {
        let original = L10n.preference
        defer { L10n.apply(original) }

        XCTAssertEqual(L10n.apply(.chinese), .chinese)
        XCTAssertEqual(L10n.text(.ok), "好")

        XCTAssertEqual(L10n.apply(.english), .english)
        XCTAssertEqual(L10n.text(.ok), "OK")

        // `.system` 解析成两者之一，具体是哪个取决于运行测试的机器。
        XCTAssertTrue([Language.chinese, .english].contains(L10n.apply(.system)))
    }

    func testFormattingSubstitutesArguments() {
        let original = L10n.preference
        defer { L10n.apply(original) }

        L10n.apply(.chinese)
        XCTAssertTrue(L(.helperConnectFailed, "连不上").contains("连不上"))

        L10n.apply(.english)
        let english = L(.helperConnectFailed, "unreachable")
        XCTAssertTrue(english.contains("unreachable"))
        XCTAssertFalse(english.contains("%@"), "参数没被替换掉：\(english)")

        // 换语序的那几条要真的按位置替换，而不是按出现顺序。
        let failure = L(.cliLinkSystemError, in: .english, "symlink", "File exists")
        XCTAssertTrue(failure.hasPrefix("symlink"), failure)
        XCTAssertTrue(failure.hasSuffix("File exists"), failure)
    }

    /// 语言标签与 C ABI 的 `tk_language_t` 对齐 —— 错位的话 GUI 是一种语言、
    /// 库日志是另一种，而且没有任何报错。
    func testCValueMatchesCABI() {
        XCTAssertEqual(Language.english.cValue, 0)
        XCTAssertEqual(Language.chinese.cValue, 1)
    }

    /// 语言菜单里的选项永远写母语名字，不跟随界面语言翻译。
    func testLanguageNamesAreNotTranslated() {
        XCTAssertEqual(L10nKey.languageChinese.localizations.chinese,
                       L10nKey.languageChinese.localizations.english)
        XCTAssertEqual(L10nKey.languageEnglish.localizations.chinese,
                       L10nKey.languageEnglish.localizations.english)
    }
}
