import Foundation
import TetherKitIPC

/// 到 GitHub Releases 查有没有更新的版本。
///
/// ★ 只「检查 + 引导」，绝不自动下载替换 ★
///   本项目免证书分发（ad-hoc 签名）。自更新（Sparkle 那类）要下载新 .app 替换
///   自己，下载物带 quarantine，替换完的 App 会被 Gatekeeper 直接拦死 ——
///   和不能走 Cask 是同一堵墙。真正的更新通道是 `brew upgrade` 或源码重编，
///   App 只负责发现新版、把命令递到用户手边。
///
/// ★ 隐私 ★
///   只请求 GitHub 的公开 REST API（releases/latest），不携带任何本机信息。
///   自动检查每天至多一次，失败静默；
///   `defaults write com.tetherkit.app updateCheckDisabled -bool YES` 可彻底关掉。
enum UpdateChecker {
    struct Release: Equatable, Sendable {
        /// 去掉 v 前缀后的版本号，如 "0.2.0"。
        let version: String
        /// Release 页，引导用户去看更新说明 / 下载。
        let pageURL: URL
    }

    enum Failure: LocalizedError {
        case noReleases
        case badStatus(Int)
        case malformedPayload

        var errorDescription: String? {
            switch self {
            case .noReleases:
                return L(.updateNoReleases)
            case .badStatus(let code):
                return L(.updateHTTPStatus, code)
            case .malformedPayload:
                return L(.updateBadResponse)
            }
        }
    }

    /// 当前 App 的版本号，来自 Info.plist（build-gui.sh 从 CMakeLists 注入）。
    /// `swift run` 的裸可执行文件没有 bundle，返回 nil —— 开发构建不做检查。
    static var currentVersion: String? {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String
    }

    /// 取最新发布。404 = 从未发布过；其余非 200 一律按失败处理。
    static func fetchLatestRelease() async throws -> Release {
        var request = URLRequest(url: endpoint)
        request.timeoutInterval = 10
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")

        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw Failure.malformedPayload }
        guard http.statusCode != 404 else { throw Failure.noReleases }
        guard http.statusCode == 200 else { throw Failure.badStatus(http.statusCode) }

        let payload = try JSONDecoder().decode(Payload.self, from: data)
        // The URL is opened with NSWorkspace, so never trust the response to
        // pick the scheme or host (file://, custom URL schemes, look-alikes).
        guard let pageURL = URL(string: payload.htmlURL), pageURL.scheme == "https",
              pageURL.host == "github.com" else { throw Failure.malformedPayload }
        return Release(version: normalize(payload.tagName), pageURL: pageURL)
    }

    /// candidate 是否比 current 新。逐段数值比较，缺位按 0，非数字段按 0 ——
    /// 解析不了宁可判「不新」，也不要为一个畸形 tag 弹更新提示。
    static func isNewer(_ candidate: String, than current: String) -> Bool {
        let lhs = components(of: candidate)
        let rhs = components(of: current)
        for index in 0..<max(lhs.count, rhs.count) {
            let l = index < lhs.count ? lhs[index] : 0
            let r = index < rhs.count ? rhs[index] : 0
            if l != r { return l > r }
        }
        return false
    }

    // MARK: - 实现

    /// `owner/repo` whose GitHub releases carry the DMGs. Read from the
    /// `TetherKitUpdateRepository` Info.plist key so a fork publishes to (and
    /// checks) its own releases without code changes.
    static var repository: String {
        let configured = Bundle.main.object(forInfoDictionaryKey: "TetherKitUpdateRepository")
            as? String
        let value = configured?.trimmingCharacters(in: .whitespaces) ?? ""
        // Only accept a plain "owner/repo"; anything else falls back.
        let valid = value.range(of: "^[A-Za-z0-9-]+/[A-Za-z0-9._-]+$",
                                options: .regularExpression) != nil
        return valid ? value : "AA-EION/TetherKitNext"
    }

    static var projectURL: URL? { URL(string: "https://github.com/\(repository)") }

    private static var endpoint: URL {
        URL(string: "https://api.github.com/repos/\(repository)/releases/latest")!
    }

    private struct Payload: Decodable {
        let tagName: String
        let htmlURL: String

        enum CodingKeys: String, CodingKey {
            case tagName = "tag_name"
            case htmlURL = "html_url"
        }
    }

    /// 去掉 tag 常见的 v/V 前缀。
    private static func normalize(_ tag: String) -> String {
        var tag = tag
        if tag.hasPrefix("v") || tag.hasPrefix("V") { tag.removeFirst() }
        return tag
    }

    private static func components(of version: String) -> [Int] {
        normalize(version).split(separator: ".").map { Int($0) ?? 0 }
    }
}
