import Foundation
import Security

/// Code-signing identity checks for the XPC connection between the app and the
/// privileged daemon.
///
/// ★ Why this exists ★
///
///   The daemon runs as root and listens on a Mach service that any local
///   process can look up. Before this, *who* connected was never checked —
///   safety rested entirely on each privileged call carrying an admin
///   AuthorizationRef. That still holds and is still enforced, but a
///   Developer ID build can do strictly better: refuse the connection outright
///   unless the peer is TetherKitNext.app signed by the same team. This is Apple's
///   documented pattern for privileged helpers
///   (NSXPCConnection.setCodeSigningRequirement, validated against the
///   peer's audit token, so it is not spoofable by PID reuse).
///
/// ★ Why the requirement is derived at runtime ★
///
///   The Team ID is read from the running binary's own signature instead of
///   being compiled in, so a fork signed with a different Developer ID works
///   without source changes, and ad-hoc development builds (no Team ID) fall
///   back to the authorization-only model instead of locking themselves out.
public enum CodeSigning {
    /// Team identifier of the running process's signature, or nil for ad-hoc /
    /// unsigned builds.
    public static let currentTeamIdentifier: String? = {
        var code: SecCode?
        guard SecCodeCopySelf([], &code) == errSecSuccess, let code else { return nil }
        var staticCode: SecStaticCode?
        guard SecCodeCopyStaticCode(code, [], &staticCode) == errSecSuccess,
              let staticCode else { return nil }
        var info: CFDictionary?
        guard SecCodeCopySigningInformation(
                  staticCode, SecCSFlags(rawValue: kSecCSSigningInformation), &info)
                  == errSecSuccess,
              let dictionary = info as? [String: Any] else { return nil }
        return dictionary[kSecCodeInfoTeamIdentifier as String] as? String
    }()

    /// Requirement a peer must satisfy: signed by Apple-issued Developer ID
    /// (or Apple Development) certificate of `teamIdentifier`, with the given
    /// bundle identifier.
    public static func requirement(identifier: String, teamIdentifier: String) -> String {
        // `anchor apple generic` = chain ends in Apple's root CA;
        // `certificate leaf[subject.OU]` = the team the leaf cert was issued to.
        "anchor apple generic and identifier \"\(identifier)\" "
            + "and certificate leaf[subject.OU] = \"\(teamIdentifier)\""
    }

    /// Requirement the daemon places on connecting clients, or nil when this
    /// build is not team-signed (development).
    public static var clientRequirement: String? {
        currentTeamIdentifier.map {
            requirement(identifier: HelperConstants.appBundleIdentifier, teamIdentifier: $0)
        }
    }

    /// Requirement the app places on the daemon it talks to, or nil when this
    /// build is not team-signed. The daemon binary lives inside the app bundle
    /// and carries its own identifier (the Mach service name).
    public static var daemonRequirement: String? {
        currentTeamIdentifier.map {
            requirement(identifier: HelperConstants.machServiceName, teamIdentifier: $0)
        }
    }

    /// Whether `requirement` compiles. Used by tests; a typo in the requirement
    /// language would otherwise surface only as "every connection rejected".
    public static func isValidRequirement(_ requirement: String) -> Bool {
        var compiled: SecRequirement?
        return SecRequirementCreateWithString(requirement as CFString, [], &compiled)
            == errSecSuccess && compiled != nil
    }
}
