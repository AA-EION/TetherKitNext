import Foundation

/// A set of constants fixed by agreement between the App and the helper.
///
/// Made a separate file so that the question "when one name changes, where else must be changed in sync" has exactly one answer --
/// these strings appear at once in the LaunchDaemon's plist, the install scripts, and the code on both ends,
/// and any one out of sync manifests as "cannot connect to the helper", with no useful error whatsoever.
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

    /// The authorization right required by privileged operations.
    ///
    /// * Why use the system built-in system.privilege.admin rather than a custom right *
    ///   A custom right must first be written into the policy database with AuthorizationRightSet, which itself needs
    ///   administrator privileges -- hence the chicken-and-egg problem of "installing the authorization needs authorization".
    ///   The rule of system.privilege.admin is authenticate-admin, popping up exactly the
    ///   password / Touch ID dialog we want, and the semantics fit too: "this is an operation that needs administrator identity".
    public static let privilegedRightName = "system.privilege.admin"

    /// The revision number of the XPC interface. **Add one every time TetherKitNextHelperProtocol is changed.**
    ///
    /// Why it is needed: the helper is installed in a system directory, and if the helper is forgotten when upgrading the App,
    /// the method signatures on the two ends will not match -- the symptom is a call hanging or a direct crash, with no hint that it is a version problem.
    /// With this number, as soon as the App connects it can detect the mismatch and explicitly tell the user "please reinstall the privileged component".
    ///
    /// Revision history:
    ///   1 -- initial version
    ///   2 -- the reply of privileged methods changed from (String?) to (String?, Bool), to distinguish authorization failures
    ///   3 -- added setLanguage, so the helper's prompts and library logs follow the UI language
    ///   4 —— SMAppService daemon (new label); adds setCommandLineToolInstalled
    ///   5 —— privileged calls may carry an empty authorization: a team-signed
    ///        app has already confirmed the user with Touch ID / the login
    ///        password, and the daemon accepts it for admin users only
    public static let protocolRevision = 5

    /// Encodes the revision number into the version string.
    ///
    /// Deliberately reuses the existing `helperVersion` method rather than adding a new one -- adding a method is itself
    /// a protocol change, and an old helper simply does not have it, which would bring us back to "mismatched and undetectable".
    /// Only a method that even an old helper will certainly answer can reliably identify an old helper.
    ///
    /// Format: `revision|version|build`. The trailing build description (which
    /// starts with the git build ID) was added so builds that share a version
    /// number can be told apart; older daemons omit it.
    public static func encodeVersion(_ version: String, build: String = "") -> String {
        build.isEmpty ? "\(protocolRevision)|\(version)" : "\(protocolRevision)|\(version)|\(build)"
    }

    /// Parses the version string. The string returned by an old helper has no separator, in which case the revision number is recorded as 0.
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

    /// Extracts the semantic version number from the library's version string:
    /// `"TetherKitNext 0.1.4 (C++23, macOS 13.3+)"` -> `"0.1.4"`.
    ///
    /// * Why not compare the whole string directly *
    ///   Besides the version number, the string also carries the C++ standard and the minimum macOS version -- those are the **build configuration**,
    ///   not the version. If the whole string were the criterion, rebuilding once with different compile options would pop up a
    ///   false alarm of "component needs updating", and clicking it would change nothing for the user.
    ///
    /// When it cannot be extracted (no dotted number in the string) fall back to the whole string: better a false alarm than a missed one -- a missed alarm
    /// means the user keeps running the pre-upgrade library without knowing it.
    public static func semanticVersion(of text: String) -> String {
        // There must be at least one dot, otherwise something like "C++23" would also be taken as a version number.
        guard let range = text.range(of: "[0-9]+(\\.[0-9]+)+", options: .regularExpression) else {
            return text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return String(text[range])
    }

    /// Timeout of XPC calls (seconds).
    ///
    /// 30 seconds is used because the slowest call is the DHCP configuration: the library waits up to 10 seconds for a lease internally,
    /// plus the USB handshake and NIC creation, leaving a 3x margin.
    public static let requestTimeout: TimeInterval = 30
}
