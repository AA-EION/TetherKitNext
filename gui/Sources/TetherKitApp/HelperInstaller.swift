import Foundation
import ServiceManagement
import TetherKitIPC

/// Registers the privileged daemon with launchd through SMAppService.
///
/// ★ Why SMAppService ★
///
///   The previous installer ran a setuid-root copy of the helper through
///   AuthorizationExecuteWithPrivileges (deprecated since macOS 10.7, looked up
///   via dlsym) and copied binaries into /Library. SMAppService (macOS 13+) is
///   Apple's supported mechanism for this:
///     * the daemon runs in place from the signed, notarized app bundle —
///       nothing root-owned is copied anywhere, so there is no install/upgrade
///       drift between the app and the daemon;
///     * the user approves it once in System Settings › Login Items, and can
///       see and revoke it there;
///     * uninstalling the app (dragging it to the Trash) removes the daemon.
///
///   The old doc (GUI-SPIKE §3.3) rejected SMAppService because source builds
///   produce a different cdhash on every machine. That constraint went away
///   with the move to a signed DMG distribution.
enum HelperInstaller {
    enum Failure: LocalizedError {
        /// Running from the DMG or from an App Translocation mount. launchd
        /// refuses bundles on read-only/randomized paths, and a daemon pointing
        /// into a DMG would vanish on eject anyway.
        case mustMoveToApplications
        case registrationFailed(String)

        var errorDescription: String? {
            switch self {
            case .mustMoveToApplications: return L(.moveToApplicationsRequired)
            case .registrationFailed(let reason): return L(.helperRegisterFailed, reason)
            }
        }
    }

    static var service: SMAppService {
        SMAppService.daemon(plistName: HelperConstants.daemonPlistName)
    }

    static var status: SMAppService.Status { service.status }

    /// True when the user still has to flip the switch in System Settings.
    static var needsApproval: Bool { status == .requiresApproval }

    /// Non-nil when the app is running from a location launchd cannot use.
    static var locationProblem: Failure? {
        let path = Bundle.main.bundleURL.resolvingSymlinksInPath().path
        if path.hasPrefix("/Volumes/") || path.contains("/AppTranslocation/") {
            return .mustMoveToApplications
        }
        return nil
    }

    /// Registers (or re-registers) the daemon.
    ///
    /// Returns normally when the daemon is enabled *or* awaiting approval; the
    /// caller checks `needsApproval` and guides the user to System Settings.
    static func register() throws {
        if let problem = locationProblem { throw problem }
        do {
            try service.register()
        } catch {
            // "Operation not permitted" here means "registered, awaiting user
            // approval" — not a failure from the user's point of view.
            if status == .requiresApproval || status == .enabled { return }
            throw Failure.registrationFailed(error.localizedDescription)
        }
    }

    /// Restarts the daemon from the current app bundle, e.g. after the app was
    /// replaced by a newer version while the old daemon kept running.
    static func reregister() async throws {
        try? await service.unregister()
        try register()
    }

    static func unregister() async throws {
        do {
            try await service.unregister()
        } catch {
            if status == .notRegistered { return }
            throw Failure.registrationFailed(error.localizedDescription)
        }
    }

    static func openApprovalSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }
}

/// State of the `/usr/local/bin/tetherkit-cli` link, read as the normal user
/// (the directory is world-readable; only changing it needs the daemon).
enum CommandLineToolState: Equatable {
    case notInstalled
    case installed
    /// Something else occupies the path (another copy of TetherKit, Homebrew).
    case occupied(String)

    static var current: CommandLineToolState {
        let linkPath = HelperConstants.commandLineToolLinkPath
        guard let target = try? FileManager.default.destinationOfSymbolicLink(atPath: linkPath)
        else {
            return FileManager.default.fileExists(atPath: linkPath)
                ? .occupied(linkPath) : .notInstalled
        }
        let ours = Bundle.main.bundleURL
            .appendingPathComponent("Contents")
            .appendingPathComponent(HelperConstants.commandLineToolBundlePath)
            .resolvingSymlinksInPath().path
        return URL(fileURLWithPath: target).resolvingSymlinksInPath().path == ours
            ? .installed : .occupied(target)
    }
}
