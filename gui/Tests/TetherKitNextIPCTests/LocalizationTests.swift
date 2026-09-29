import XCTest

@testable import TetherKitNextIPC

/// Consistency check of the message table.
///
/// * The sole reason these cases exist *
///
///   `L(...)` goes through `String(format:)`, and the format string comes from a runtime table lookup -- the compiler cannot manage it.
///   When the placeholders of the two languages do not match, `String(format:)` **does not report an error**: it quietly interprets
///   an argument that is not of that type at all by the type it reads. `%@` meeting an integer dereferences the integer
///   as a pointer and crashes directly; `%ld` meeting a string prints an address. Both are
///   "translation written wrong -> blows up at runtime on the user's machine", and moreover the Chinese version is fine while the English version blows up.
///
///   So this safeguard must be made up here. `L10nKey` is CaseIterable, so newly added messages
///   are covered automatically, with no manual registration.
final class LocalizationTests: XCTestCase {

    /// One replacement field: the position (the number of an explicit `%1$`, or by order of appearance if implicit) + the conversion type.
    private struct Placeholder: Equatable, CustomStringConvertible {
        let position: Int
        let conversion: String

        var description: String { "#\(position):%\(conversion)" }
    }

    /// Parses a printf-style format string.
    ///
    /// Covers only the subset this table really uses: `%@`, `%ld`, `%lx`, `%f` and their forms with width/precision/position,
    /// plus the `%%` escape.
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
            if characters[index] == "%" {  // `%%` escape
                index += 1
                continue
            }

            // Explicit position: `3$`
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

            // Flags, width, precision -- all skipped, since they do not affect the argument type.
            while index < characters.count,
                  "0123456789.-+ #'".contains(characters[index]) {
                index += 1
            }
            // Length modifiers (l / ll / h / z) are part of the type and must be kept.
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

    /// The core case of this file.
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

            // Positions must be consecutive 1...n. With a gap String(format:) can still render,
            // but that means some argument is ignored by both languages at once, which is almost always a mistake.
            for (offset, placeholder) in chinesePlaceholders.enumerated() {
                XCTAssertEqual(placeholder.position, offset + 1,
                               "\(key.rawValue) 的占位符编号不连续：\(chinesePlaceholders)")
            }
        }
    }

    /// Integers are always `%ld`: `%d` takes only the low 32 bits of a 64-bit argument, and the Swift side passes `Int`.
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

        // `.system` resolves to one of the two, and which one depends on the machine running the tests.
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

        // The ones that change word order must really substitute by position, not by order of appearance.
        let failure = L(.cliLinkSystemError, in: .english, "symlink", "File exists")
        XCTAssertTrue(failure.hasPrefix("symlink"), failure)
        XCTAssertTrue(failure.hasSuffix("File exists"), failure)
    }

    /// The language tags are aligned with the C ABI's `tk_language_t` -- if misaligned the GUI would be in one language and
    /// the library logs in another, with no error whatsoever.
    func testCValueMatchesCABI() {
        XCTAssertEqual(Language.english.cValue, 0)
        XCTAssertEqual(Language.chinese.cValue, 1)
    }

    /// The options in the language menu are always written as native-language names and are not translated with the UI language.
    func testLanguageNamesAreNotTranslated() {
        XCTAssertEqual(L10nKey.languageChinese.localizations.chinese,
                       L10nKey.languageChinese.localizations.english)
        XCTAssertEqual(L10nKey.languageEnglish.localizations.chinese,
                       L10nKey.languageEnglish.localizations.english)
    }
}
