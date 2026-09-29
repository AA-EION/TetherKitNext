import Foundation
import TetherKitNextIPC

/// Checks GitHub Releases for a newer version.
///
/// * Only "check + guide", never automatically download and replace *
///   This project is distributed without a certificate (ad-hoc signing). Self-updating (the Sparkle kind) has to download a new .app to replace
///   itself, the download carries quarantine, and the replaced App would be blocked dead by Gatekeeper --
///   the same wall as why Cask cannot be used. The real update channels are `brew upgrade` or a source rebuild,
///   and the App is only responsible for discovering the new version and putting the command within the user's reach.
///
/// * Privacy *
///   Only GitHub's public REST API (releases/latest) is requested, carrying no local machine information.
///   The automatic check runs at most once a day and fails silently;
///   `defaults write com.tetherkitnext.app updateCheckDisabled -bool YES` turns it off completely.
enum UpdateChecker {
    struct Release: Equatable, Sendable {
        /// The version number with the v prefix removed, such as "0.2.0".
        let version: String
        /// The Release page, guiding the user to read the release notes / download.
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

    /// The current App's version number, from Info.plist (injected by build-gui.sh from CMakeLists).
    /// The bare executable of `swift run` has no bundle, so it returns nil -- development builds do no checking.
    static var currentVersion: String? {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String
    }

    /// Gets the latest release. 404 = never released; any other non-200 is treated as failure.
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

    /// Whether candidate is newer than current. Compared segment by segment numerically, with missing segments as 0 and non-numeric segments as 0 --
    /// if it cannot be parsed, better to judge "not newer" than to pop up an update hint for a malformed tag.
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

    // MARK: - Implementation

    /// `owner/repo` whose GitHub releases carry the DMGs. Read from the
    /// `TetherKitNextUpdateRepository` Info.plist key so a fork publishes to (and
    /// checks) its own releases without code changes.
    static var repository: String {
        let configured = Bundle.main.object(forInfoDictionaryKey: "TetherKitNextUpdateRepository")
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

    /// Strips the common v/V prefix of a tag.
    private static func normalize(_ tag: String) -> String {
        var tag = tag
        if tag.hasPrefix("v") || tag.hasPrefix("V") { tag.removeFirst() }
        return tag
    }

    private static func components(of version: String) -> [Int] {
        normalize(version).split(separator: ".").map { Int($0) ?? 0 }
    }
}
