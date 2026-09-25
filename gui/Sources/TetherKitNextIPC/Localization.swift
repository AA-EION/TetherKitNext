import Foundation

// 界面文案的语言切换。
//
// ★ 为什么不用 Localizable.strings / String(localized:) ★
//
//   标准做法要把 .lproj 目录打进 bundle，而本工程有三个互相独立的产物形态：
//     * `swift build` 出来的裸可执行文件（开发时直接跑）；
//     * Scripts/build-gui.sh 手工拼出来的 TetherKitNext.app；
//     * 装到 /Library/PrivilegedHelperTools 的 **裸** helper 可执行文件。
//   最后这个是致命的：helper 旁边没有、也不该有一个资源 bundle，而它同样要
//   产生给用户看的文字（提示、错误）。走 bundle 的话 helper 只能永远输出
//   开发语言，或者得在安装脚本里再搬一套资源过去。
//
//   把文案编进二进制就没有这些问题：三种形态行为完全一致，安装脚本不用动，
//   也不存在「装到别的机器上找不到 .lproj 于是全变英文」这类运行期故障。
//   顺带和 C++ 侧（include/tetherkitnext/common/messages.def）是同一个心智模型。
//
//   代价是用不上 Xcode 的字符串目录编辑器 —— 而这个工程本来就没有 Xcode
//   工程文件，代价为零。
//
// ★ 加一条文案 ★
//
//   1. 在 LocalizedStrings.swift 的 `L10nKey` 里加一个 case；
//   2. 在同文件的 `localizations` switch 里补上中英两版。
//   switch 是穷尽的，**漏了编译不过** —— 这比 C++ 那边的 X-macro 还强一档。

/// 用户能选的语言偏好。`system` 表示跟随 macOS。
public enum LanguagePreference: String, CaseIterable, Codable, Sendable {
    case system
    case chinese
    case english
}

/// 实际生效的语言。偏好为 `system` 时由 `L10n` 解析成这两者之一。
public enum Language: String, CaseIterable, Codable, Sendable {
    case chinese
    case english

    /// 与 C ABI 的 `tk_language_t` 对齐（TK_LANGUAGE_ENGLISH = 0、CHINESE = 1）。
    public var cValue: Int32 { self == .chinese ? 1 : 0 }
}

/// 文案查表与语言状态。
///
/// 状态是**进程级**的：日志与提示会从多个线程产生，做成线程局部只会让同一次
/// 会话的输出出现两种语言。读远多于写，用一把小锁足够 —— 每次查表多一次
/// 无争用的加锁，相对 SwiftUI 一次渲染的开销可以忽略。
public enum L10n {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var storedPreference: LanguagePreference = .system
    nonisolated(unsafe) private static var storedLanguage: Language = resolve(.system)

    /// 当前生效的语言。
    public static var language: Language {
        lock.lock()
        defer { lock.unlock() }
        return storedLanguage
    }

    /// 当前的偏好设置（可能是 `.system`）。
    public static var preference: LanguagePreference {
        lock.lock()
        defer { lock.unlock() }
        return storedPreference
    }

    /// 应用一个偏好，并返回解析后实际生效的语言。
    ///
    /// 调用方拿到返回值后**还要**把它推给 libtetherkitnext（见
    /// `TetherKitNextLibrary.setLanguage`），否则库产生的日志会和界面语言不一致。
    @discardableResult
    public static func apply(_ preference: LanguagePreference) -> Language {
        let resolved = resolve(preference)
        lock.lock()
        storedPreference = preference
        storedLanguage = resolved
        lock.unlock()
        return resolved
    }

    /// macOS 当前的首选语言落到我们支持的两种上。
    ///
    /// 看 `Locale.preferredLanguages` 而不是 `Locale.current`：后者受区域格式
    /// 设置影响（有人把地区设成中国但界面语言是英文），前者才是「界面该用哪种
    /// 语言」的那份列表。
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

    /// 取一条文案在当前语言下的原文（未做参数替换）。
    public static func text(_ key: L10nKey) -> String {
        let (chinese, english) = key.localizations
        return language == .chinese ? chinese : english
    }

    /// 取一条文案在**指定**语言下的原文。测试用它逐语言核对占位符。
    public static func text(_ key: L10nKey, in language: Language) -> String {
        let (chinese, english) = key.localizations
        return language == .chinese ? chinese : english
    }
}

/// 取一条文案，并按 `String(format:)` 的规则替换参数。
///
/// 做成全局函数而不是 `L10n.text(...)`：调用点有两百多处、绝大多数在 SwiftUI 的
/// 视图体里，那里每多一个字都会挤掉真正重要的布局代码。名字短到一个字母也不会
/// 歧义 —— 见到 `L(` 就知道是「这里有一条要翻译的文案」。
///
/// 占位符用 printf 风格（`%@` 字符串、`%d` 整数、`%.1f` 浮点）。需要换语序时
/// 用带位置的形式：`%1$@`、`%2$d`。
public func L(_ key: L10nKey, _ arguments: CVarArg...) -> String {
    let pattern = L10n.text(key)
    return arguments.isEmpty ? pattern : String(format: pattern, arguments: arguments)
}

/// 同上，但指定语言。给 helper 用 —— 它按 App 推过来的语言渲染，而不是按
/// 自己的进程状态（root 进程没有「用户偏好」这回事）。
public func L(_ key: L10nKey, in language: Language, _ arguments: CVarArg...) -> String {
    let pattern = L10n.text(key, in: language)
    return arguments.isEmpty ? pattern : String(format: pattern, arguments: arguments)
}
