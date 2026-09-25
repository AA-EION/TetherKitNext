import Foundation

/// App 与 helper 之间约定死的一组常量。
///
/// 单独成文件是为了让「改一个名字要同步改哪些地方」这件事只有一个答案 ——
/// 这些字符串同时出现在 LaunchDaemon 的 plist、安装脚本和两端代码里，
/// 任何一处不同步的表现都是「连不上 helper」，且没有任何有用的报错。
public enum HelperConstants {
    /// Mach service name and launchd label of the privileged daemon.
    ///
    /// Must match `Label` / `MachServices` in
    /// Resources/com.tetherkitnext.helperd.plist (embedded in the app at
    /// Contents/Library/LaunchDaemons and registered through SMAppService).
    ///
    /// Deliberately different from the legacy `com.tetherkit.helper` label:
    /// launchd refuses to register a second job under a label that is still
    /// loaded, and the legacy daemon (installed by older builds via
    /// AuthorizationExecuteWithPrivileges) may be. The new daemon removes the
    /// legacy one on first start — see LegacyHelper.
    public static let machServiceName = "com.tetherkitnext.helperd"

    /// File name of the daemon plist inside Contents/Library/LaunchDaemons,
    /// as SMAppService.daemon(plistName:) expects it.
    public static let daemonPlistName = "com.tetherkitnext.helperd.plist"

    /// Bundle identifier of TetherKitNext.app. The daemon only accepts XPC
    /// connections from code signed with this identifier (and its own Team ID).
    public static let appBundleIdentifier = "com.tetherkitnext.app"

    /// Where the command-line tool is linked for terminal use. /usr/local/bin is
    /// in the default PATH of every shell on macOS (/etc/paths) and survives
    /// `sudo`, so a symlink there makes `tetherkitnext-cli` and
    /// `sudo tetherkitnext-cli` work without touching shell profiles.
    public static let commandLineToolLinkPath = "/usr/local/bin/tetherkitnext-cli"

    /// Location of the CLI inside the app bundle, relative to Contents/.
    public static let commandLineToolBundlePath = "MacOS/tetherkitnext-cli"

    /// Legacy (pre-SMAppService) installation, removed on upgrade.
    public enum Legacy {
        public static let label = "com.tetherkit.helper"
        public static let executablePath = "/Library/PrivilegedHelperTools/com.tetherkit.helper"
        public static let launchDaemonPlistPath = "/Library/LaunchDaemons/com.tetherkit.helper.plist"
        public static let toolsDirectory = "/Library/PrivilegedHelperTools"
    }

    /// 特权操作所要求的授权权利。
    ///
    /// ★ 为什么用系统内置的 system.privilege.admin，而不是自定义权利 ★
    ///   自定义权利要先用 AuthorizationRightSet 写进策略数据库，而那本身就需要
    ///   管理员权限 —— 于是就有了「安装授权需要授权」的先有鸡还是先有蛋问题。
    ///   system.privilege.admin 的规则是 authenticate-admin，弹的正是我们想要的
    ///   密码 / Touch ID 框，语义也贴切：「这是一次需要管理员身份的操作」。
    public static let privilegedRightName = "system.privilege.admin"

    /// XPC 接口的修订号。**每次改动 TetherKitNextHelperProtocol 都要加一。**
    ///
    /// 为什么需要它：helper 是装在系统目录里的，升级 App 时如果忘了重装 helper，
    /// 两端的方法签名就对不上 —— 表现是调用卡住或者直接崩，完全看不出是版本问题。
    /// 有了这个号，App 一连上就能发现不匹配并明确告诉用户「请重新安装特权组件」。
    ///
    /// 修订历史：
    ///   1 —— 初版
    ///   2 —— 特权方法的应答从 (String?) 改成 (String?, Bool)，区分授权失败
    ///   3 —— 新增 setLanguage，让 helper 的提示与库日志跟随界面语言
    ///   4 —— SMAppService daemon (new label); adds setCommandLineToolInstalled
    public static let protocolRevision = 4

    /// 把修订号编进版本串。
    ///
    /// 刻意复用现成的 `helperVersion` 方法而不是新增一个 —— 新增方法本身就是
    /// 一次协议变更，旧 helper 根本没有它，那就又回到了「对不上还查不出来」。
    /// 用旧 helper 也一定会应答的这个方法，才能可靠地识别出旧 helper。
    ///
    /// Format: `revision|version|build`. The trailing build description (which
    /// starts with the git build ID) was added so builds that share a version
    /// number can be told apart; older daemons omit it.
    public static func encodeVersion(_ version: String, build: String = "") -> String {
        build.isEmpty ? "\(protocolRevision)|\(version)" : "\(protocolRevision)|\(version)|\(build)"
    }

    /// 解析版本串。旧 helper 返回的串里没有分隔符，此时修订号记作 0。
    public static func decodeVersion(_ encoded: String)
        -> (revision: Int, version: String, build: String) {
        guard let separator = encoded.firstIndex(of: "|"),
              let revision = Int(encoded[encoded.startIndex..<separator]) else {
            return (0, encoded, "")
        }
        let rest = encoded[encoded.index(after: separator)...]
        guard let second = rest.firstIndex(of: "|") else {
            return (revision, String(rest), "")
        }
        return (revision, String(rest[rest.startIndex..<second]),
                String(rest[rest.index(after: second)...]))
    }

    /// The build ID (`"1a2b3c4d5e"`) from a library build description
    /// (`"build 1a2b3c4d5e, Release, …"`), or nil when absent.
    public static func buildID(of description: String) -> String? {
        guard description.hasPrefix("build ") else { return nil }
        let id = description.dropFirst("build ".count).prefix { $0 != "," }
        return id.isEmpty ? nil : String(id)
    }

    /// 从库的版本串里取出语义化版本号：
    /// `"TetherKitNext 0.1.4 (C++23, macOS 13.3+)"` → `"0.1.4"`。
    ///
    /// ★ 为什么不直接比整串 ★
    ///   串里除了版本号还带着 C++ 标准与最低 macOS 版本 —— 那是**构建配置**，
    ///   不是版本。拿整串当判据的话，换个编译选项重建一次就会冒出一个
    ///   「组件该更新了」的假警报，而用户点下去什么也不会变。
    ///
    /// 取不到（串里没有带点的数字）时退回整串：宁可误报也不要漏报 —— 漏报
    /// 意味着用户一直在跑升级前的那份库，且毫不知情。
    public static func semanticVersion(of text: String) -> String {
        // 至少要有一个点，否则 "C++23" 这种也会被当成版本号。
        guard let range = text.range(of: "[0-9]+(\\.[0-9]+)+", options: .regularExpression) else {
            return text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return String(text[range])
    }

    /// XPC 调用的超时（秒）。
    ///
    /// 取 30 秒是因为最慢的一次调用是 DHCP 配置：库内部最多等 10 秒租约，
    /// 加上 USB 握手与网卡创建，留 3 倍余量。
    public static let requestTimeout: TimeInterval = 30
}
