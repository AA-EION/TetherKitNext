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
    case chinese
    case english
}

/// The language actually in effect. When the preference is `system`, `L10n` resolves it into one of these two.
public enum Language: String, CaseIterable, Codable, Sendable {
    case chinese
    case english

    /// Aligned with the C ABI's `tk_language_t` (TK_LANGUAGE_ENGLISH = 0, CHINESE = 1).
    public var cValue: Int32 { self == .chinese ? 1 : 0 }
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

    /// Maps macOS's current preferred language onto one of the two we support.
    ///
    /// It looks at `Locale.preferredLanguages` rather than `Locale.current`: the latter is affected by regional format
    /// settings (someone sets the region to China but the UI language to English), and the former is the list that answers "which
    /// language the UI should use".
    public static var systemLanguage: Language {
        let preferred = Locale.preferredLanguages.first ?? "en"
        return preferred.lowercased().hasPrefix("zh") ? .chinese : .english
    }

    private static func resolve(_ preference: LanguagePreference) -> Language {
        switch preference {
        case .system: return systemLanguage
        case .chinese: return .chinese
        case .english: return .english
        }
    }

    /// Gets the original text of a message in the current language (without argument substitution).
    public static func text(_ key: L10nKey) -> String {
        let (chinese, english) = key.localizations
        return language == .chinese ? chinese : english
    }

    /// Gets the original text of a message in the **specified** language. Tests use it to check placeholders language by language.
    public static func text(_ key: L10nKey, in language: Language) -> String {
        let (chinese, english) = key.localizations
        return language == .chinese ? chinese : english
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
