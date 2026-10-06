import Foundation

// Language switching for UI messages.
//
// * Why not Localizable.strings / String(localized:) *
//
//   The standard approach packs .lproj directories into the bundle, while this project has three mutually independent product forms:
//     * the bare executable produced by `swift build` (run directly during development);
//     * the TetherKitNext.app assembled by hand by Scripts/build-gui.sh;
//     * the **bare** helper executable installed to /Library/PrivilegedHelperTools.
//   The last one is fatal: the helper has no resource bundle beside it and should not have one, yet it too has to
//   produce text shown to users (hints, errors). Going through a bundle, the helper could only ever output
//   the development language, or the install script would have to move another set of resources over.
//
//   Compiling the messages into the binary has none of these problems: all three forms behave identically, the install script needs no changes,
//   and there is no runtime failure of the kind "installed on another machine, .lproj not found, so everything turns English".
//   Incidentally, it is the same mental model as the C++ side (include/tetherkitnext/common/messages.def).
//
//   The cost is not being able to use Xcode's string catalog editor -- and this project has no Xcode
//   project file to begin with, so the cost is zero.
//
// * Adding a message *
//
//   1. Add a case to `L10nKey` in LocalizedStrings.swift;
//   2. Fill in both the Chinese and English versions in the `localizations` switch in the same file.
//   The switch is exhaustive, so **omissions fail to compile** -- one notch stronger than the X-macro on the C++ side.

/// The language preferences a user can choose. `system` means follow macOS.
public enum LanguagePreference: String, CaseIterable, Codable, Sendable {
    case system
    case english
    case spanish
    case chinese
    case japanese
    case german
    case korean
}

/// The language actually in effect. When the preference is `system`, `L10n` resolves it into one of these.
public enum Language: String, CaseIterable, Codable, Sendable {
    case english
    case spanish
    case chinese
    case japanese
    case german
    case korean

    /// Aligned with the C ABI's `tk_language_t` (TK_LANGUAGE_ENGLISH = 0, CHINESE = 1).
    public var cValue: Int32 {
        switch self {
        case .english: return 0
        case .chinese: return 1
        default: return 0
        }
    }
}

/// Message lookup and language state.
///
/// The state is **process-wide**: logs and hints are produced from multiple threads, and making it thread-local would only make the output of a single
/// session appear in two languages. Reads far outnumber writes, and one small lock suffices -- each lookup costs one extra
/// uncontended lock acquisition, negligible relative to the cost of one SwiftUI render.
public enum L10n {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var storedPreference: LanguagePreference = .system
    nonisolated(unsafe) private static var storedLanguage: Language = resolve(.system)

    /// The language currently in effect.
    public static var language: Language {
        lock.lock()
        defer { lock.unlock() }
        return storedLanguage
    }

    /// The current preference setting (may be `.system`).
    public static var preference: LanguagePreference {
        lock.lock()
        defer { lock.unlock() }
        return storedPreference
    }

    /// Applies a preference, and returns the resolved language actually in effect.
    ///
    /// After getting the return value the caller **must also** push it to libtetherkitnext (see
    /// `TetherKitNextLibrary.setLanguage`), otherwise the logs the library produces will be inconsistent with the UI language.
    @discardableResult
    public static func apply(_ preference: LanguagePreference) -> Language {
        let resolved = resolve(preference)
        lock.lock()
        storedPreference = preference
        storedLanguage = resolved
        lock.unlock()
        return resolved
    }

    /// Directly applies a language (used by the helper daemon when informed by the client App).
    @discardableResult
    public static func apply(_ language: Language) -> Language {
        lock.lock()
        storedPreference = LanguagePreference(rawValue: language.rawValue) ?? .system
        storedLanguage = language
        lock.unlock()
        return language
    }

    /// Maps macOS's current preferred languages onto one of the supported languages.
    /// Checks preferred languages in order and defaults to English when no supported language matches.
    public static var systemLanguage: Language {
        for preferred in Locale.preferredLanguages {
            let lower = preferred.lowercased()
            if lower.hasPrefix("es") { return .spanish }
            if lower.hasPrefix("zh") { return .chinese }
            if lower.hasPrefix("ja") { return .japanese }
            if lower.hasPrefix("de") { return .german }
            if lower.hasPrefix("ko") { return .korean }
            if lower.hasPrefix("en") { return .english }
        }
        return .english
    }

    private static func resolve(_ preference: LanguagePreference) -> Language {
        switch preference {
        case .system: return systemLanguage
        case .english: return .english
        case .spanish: return .spanish
        case .chinese: return .chinese
        case .japanese: return .japanese
        case .german: return .german
        case .korean: return .korean
        }
    }

    /// Gets the original text of a message in the current language (without argument substitution).
    public static func text(_ key: L10nKey) -> String {
        return key.translation(for: language)
    }

    /// Gets the original text of a message in the **specified** language. Tests use it to check placeholders language by language.
    public static func text(_ key: L10nKey, in language: Language) -> String {
        return key.translation(for: language)
    }
}

/// Gets a message and substitutes arguments by the rules of `String(format:)`.
///
/// Made a global function rather than `L10n.text(...)`: there are over two hundred call sites, the vast majority in SwiftUI
/// view bodies, where every extra character crowds out the layout code that really matters. A name short as one letter is not
/// ambiguous either -- on seeing `L(` you know "there is a message to be translated here".
///
/// Placeholders use printf style (`%@` string, `%d` integer, `%.1f` float). When a different word order is needed
/// use the positional form: `%1$@`, `%2$d`.
public func L(_ key: L10nKey, _ arguments: CVarArg...) -> String {
    let pattern = L10n.text(key)
    return arguments.isEmpty ? pattern : String(format: pattern, arguments: arguments)
}

/// Same as above, but with a specified language. For the helper -- it renders in the language the App pushed over, rather than by
/// its own process state (a root process has no such thing as "user preferences").
public func L(_ key: L10nKey, in language: Language, _ arguments: CVarArg...) -> String {
    let pattern = L10n.text(key, in: language)
    return arguments.isEmpty ? pattern : String(format: pattern, arguments: arguments)
}
