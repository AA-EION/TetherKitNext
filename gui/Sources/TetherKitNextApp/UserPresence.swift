import Foundation
import LocalAuthentication
import TetherKitNextIPC

/// Confirms the user is at the Mac before a privileged action, with Touch ID
/// (or an Apple Watch, or the login password as fallback) instead of the
/// administrator password dialog.
///
/// ★ Why this is enough for a root daemon ★
///
///   The check itself runs in the app, so on its own it proves nothing to the
///   daemon. What makes it sound is the XPC pinning of team-signed builds: the
///   daemon only talks to TetherKitNext.app signed by our own team, so the only
///   program able to send it an "already confirmed" request is this app, and
///   this app always asks first. The daemon additionally requires the caller
///   to be an administrator (see `HelperService.authorize`), so the set of
///   people who can connect is unchanged — only how they prove it is.
///
///   Ad-hoc / development builds have no Team ID to pin, so `isAvailable` is
///   false there and the app keeps using the administrator dialog.
@MainActor
enum UserPresence {
    enum Failure: Error {
        /// The user dismissed the prompt. Not an error to show.
        case cancelled
        /// No biometrics and no password fallback (e.g. no login password set),
        /// or the prompt failed for another reason: use the admin dialog.
        case unavailable
    }

    /// One confirmation covers what follows it for this long, so Connect and
    /// the automatic network setup right after it do not ask twice.
    static let gracePeriod: TimeInterval = 5 * 60

    private static var confirmedAt: Date?

    /// Team-signed builds only (see above).
    static var isAvailable: Bool { CodeSigning.currentTeamIdentifier != nil }

    /// Asks for Touch ID / the login password unless a confirmation within the
    /// grace period already exists. `reason` completes "TetherKitNext is trying
    /// to …".
    static func confirm(reason: String) async throws {
        if let confirmedAt, Date().timeIntervalSince(confirmedAt) < gracePeriod {
            return
        }
        let context = LAContext()
        var availabilityError: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &availabilityError) else {
            throw Failure.unavailable
        }
        do {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                // @Sendable: LocalAuthentication calls this on its own queue.
                // Without it Swift 6 would infer main-actor isolation from the
                // surrounding code and trap when it runs off the main thread.
                context.evaluatePolicy(.deviceOwnerAuthentication,
                                       localizedReason: reason) { @Sendable success, error in
                    if success {
                        continuation.resume()
                    } else {
                        continuation.resume(throwing: error ?? Failure.unavailable)
                    }
                }
            }
        } catch let error as LAError
                    where [.userCancel, .appCancel, .systemCancel].contains(error.code) {
            throw Failure.cancelled
        } catch {
            throw Failure.unavailable
        }
        confirmedAt = Date()
    }

    /// Forgets the last confirmation, e.g. after the daemon is disabled.
    static func reset() {
        confirmedAt = nil
    }
}
