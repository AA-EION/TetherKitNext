import Foundation
import Security

/// Obtaining the authorization credential (App side) and verifying it (helper side).
///
/// * The two sides are deliberately placed in the same file *
///   The difference between them is only one flag (`.interactionAllowed`), and getting that one bit wrong is a
///   security hole. Written together, when one side is changed the other is right in front of you.
///
/// * One sentence that must be remembered: authorization != root *
///   A successful `AuthorizationCopyRights` **changes nothing about the process** -- neither uid nor euid
///   is touched. What it produces is only a credential saying "the user confirmed with password/fingerprint at some moment".
///   The privilege itself must have another source (here, the helper launched by launchd as root).
///
///   So the order cannot be reversed: it is not "first pop up the fingerprint to get root, then create the NIC", but "first have a resident
///   root helper, and for each operation pop up the fingerprint to get a credential and hand it to it for verification".

/// The holder of one authorization.
///
/// * Its sole reason to exist: the external form is **not the credential itself, only a reference** *
///
///   The 32 bytes produced by `AuthorizationMakeExternalForm` contain no rights information whatsoever; it is just
///   a key pointing to that authorization inside securityd. As soon as the App side releases the AuthorizationRef
///   (especially with `.destroyRights`), what is in securityd is gone --
///   and the helper's subsequent `AuthorizationCreateFromExternalForm` will fail,
///   reporting `errAuthorizationDenied (-60005)`.
///
///   So the AuthorizationRef must **live until after the XPC round trip ends**. Holding it in a class
///   and letting ARC manage it is far more reliable than hand-writing "remember to free it at the end" at every call site.
///
/// * Why it is safe to mark @unchecked Sendable *
///   HelperInstaller has to hand the token to a background queue (AEWP blocks on the authorization dialog and cannot occupy the main
///   thread). There is no mutable state here -- both stored properties are let; the AuthorizationRef itself
///   is thread-safe per the Authorization Services documentation (the real state is in the securityd
///   process, and cross-process calls are naturally serialized); and deinit is guaranteed by ARC to run only once.
public final class AuthorizationToken: @unchecked Sendable {
    /// The 32-byte external form that can be passed across processes to the helper.
    public let externalForm: Data

    private let authorization: AuthorizationRef

    init(authorization: AuthorizationRef, externalForm: Data) {
        self.authorization = authorization
        self.externalForm = externalForm
    }

    /// Briefly lends the underlying AuthorizationRef to an API that needs the thing itself (currently only
    /// `AuthorizationExecuteWithPrivileges` -- it wants the ref, not the external form).
    ///
    /// Made a scoped borrow rather than exposing a property directly: the ref's lifetime belongs to this class, and if anyone stores it
    /// outside the closure, it becomes a dangling reference once the token is released -- an error the compiler cannot catch.
    public func withReference<T>(_ body: (AuthorizationRef) throws -> T) rethrows -> T {
        try body(authorization)
    }

    deinit {
        // With .destroyRights: the credential belongs to the App, is destroyed after use, and no long-lived pass
        // is left in the process. (On the helper side, verification **must not** carry this flag,
        // otherwise it would invalidate this side's authorization too -- see AuthorizationVerifier.)
        AuthorizationFree(authorization, [.destroyRights])
    }
}

/// App side: requests authorization from the user, obtaining a credential that can be passed across processes.
public enum AuthorizationBroker {
    public enum Failure: LocalizedError {
        case userCancelled
        case denied(OSStatus)
        case internalFailure(OSStatus)

        public var errorDescription: String? {
            switch self {
            case .userCancelled:
                return L(.authorizationCancelled)
            case .denied(let status):
                return L(.authorizationDenied, Int(status))
            case .internalFailure(let status):
                return L(.authorizationSessionFailed, Int(status))
            }
        }
    }

    /// Pops up the system authorization dialog, and on success returns a token holding this authorization.
    ///
    /// * The token should be **cached and reused**, rather than fetched anew for every operation *
    ///
    ///   The measured parameters of the `system.privilege.admin` rule are `shared = false` and
    ///   `timeout = 300`:
    ///     * `shared = false` -- the credential is **not shared across AuthorizationRefs**. Every time
    ///       `AuthorizationCreate` creates a new ref, the user must authenticate again.
    ///     * `timeout = 300` -- but on the **same ref**, the credential is valid for 5 minutes.
    ///
    ///   So "connect, configure network, disconnect" each popping up a dialog is purely because we create a new ref every time.
    ///   Reusing the same token needs only one authentication within 5 minutes. This is also what Apple itself does in
    ///   EvenBetterAuthorizationSample.
    ///
    ///   After expiry the helper-side verification will fail, and the caller can discard the cache and fetch again accordingly.
    ///
    /// WARNING: **The caller must keep the token alive until after the XPC round trip ends.** Once the token is released, the authorization
    /// in securityd is gone, and when the helper restores the external form it reports `errAuthorizationDenied (-60005)`.
    ///
    /// - Parameter prompt: A sentence shown in the system authorization dialog, telling the user what this authorization is
    ///   for. If not passed, the dialog has only the bland "wants to make changes".
    ///
    /// Must be called on the main thread -- it presents UI.
    public static func requestAuthorization(
        right: String = HelperConstants.privilegedRightName,
        prompt: String? = nil
    ) throws -> AuthorizationToken {
        var authorization: AuthorizationRef?
        let createStatus = AuthorizationCreate(nil, nil, [], &authorization)
        guard createStatus == errAuthorizationSuccess, let authorization else {
            throw Failure.internalFailure(createStatus)
        }
        // On the failure path release immediately; on the success path ownership is handed to AuthorizationToken.
        var handedOff = false
        defer {
            if !handedOff {
                AuthorizationFree(authorization, [.destroyRights])
            }
        }

        let copyStatus = copyRights(authorization, right: right, prompt: prompt)
        guard copyStatus == errAuthorizationSuccess else {
            throw copyStatus == errAuthorizationCanceled
                ? Failure.userCancelled
                : Failure.denied(copyStatus)
        }

        var external = AuthorizationExternalForm()
        let externalStatus = AuthorizationMakeExternalForm(authorization, &external)
        guard externalStatus == errAuthorizationSuccess else {
            throw Failure.internalFailure(externalStatus)
        }

        handedOff = true
        return AuthorizationToken(authorization: authorization,
                                  externalForm: withUnsafeBytes(of: &external) { Data($0) })
    }

    /// Requests rights, popping up a dialog if necessary.
    ///
    /// Factored out separately because constructing the environment items needs several layers of nested `withUnsafe*` -- those buffers must live
    /// until after `AuthorizationCopyRights` returns, and if written in the main flow, a later person could easily
    /// "tidy it up in passing" into a dangling pointer.
    private static func copyRights(_ authorization: AuthorizationRef,
                                   right: String,
                                   prompt: String?) -> OSStatus {
        var name = Array(right.utf8CString)
        var promptKey = Array(kAuthorizationEnvironmentPrompt.utf8CString)
        var promptValue = Array(prompt?.utf8 ?? "".utf8)

        return name.withUnsafeMutableBufferPointer { nameBuffer in
            var rightItem = AuthorizationItem(name: nameBuffer.baseAddress!, valueLength: 0,
                                              value: nil, flags: 0)
            return withUnsafeMutablePointer(to: &rightItem) { rightPointer in
                var rights = AuthorizationRights(count: 1, items: rightPointer)

                // The App side **does** carry .interactionAllowed -- popping up the dialog is exactly what we want.
                // .preAuthorize makes the right be granted on the spot rather than at actual use,
                // so that the credential enters this ref's cache and can be reused by subsequent operations.
                let flags: AuthorizationFlags = [.extendRights, .interactionAllowed, .preAuthorize]

                guard prompt != nil else {
                    return AuthorizationCopyRights(authorization, &rights, nil, flags, nil)
                }
                return promptKey.withUnsafeMutableBufferPointer { keyBuffer in
                    promptValue.withUnsafeMutableBufferPointer { valueBuffer in
                        var promptItem = AuthorizationItem(
                            name: keyBuffer.baseAddress!,
                            valueLength: valueBuffer.count,
                            value: valueBuffer.baseAddress,
                            flags: 0)
                        return withUnsafeMutablePointer(to: &promptItem) { promptPointer in
                            var environment = AuthorizationEnvironment(count: 1,
                                                                       items: promptPointer)
                            return AuthorizationCopyRights(authorization, &rights, &environment,
                                                           flags, nil)
                        }
                    }
                }
            }
        }
    }
}

/// helper side: verifies the credential handed over by the caller.
public enum AuthorizationVerifier {
    public enum Failure: LocalizedError {
        case malformedCredential
        case restoreFailed(OSStatus)
        case rightNotHeld(OSStatus)

        public var errorDescription: String? {
            switch self {
            case .malformedCredential:
                return L(.authorizationBlobMalformed)
            case .restoreFailed(let status):
                return L(.authorizationRestoreFailed, Int(status))
            case .rightNotHeld(let status):
                return L(.authorizationRightMissing, Int(status))
            }
        }
    }

    /// Verifies that the credential really contains the specified right. Throws if it does not.
    ///
    /// * Three details that must be followed *
    ///
    ///   1. **Must never carry `.interactionAllowed`.** The daemon has no UI session; if it were really allowed to
    ///      pop up a dialog, any process able to connect to the Mach service could trigger system authorization dialogs at will to harass
    ///      the user. This step only checks "does this credential already contain this right", and acquires no new rights.
    ///
    ///   2. **`AuthorizationFree` must not carry `.destroyRights`.** The credential belongs to the App,
    ///      and the helper only borrows it for checking; carrying it would invalidate the App's authorization too.
    ///
    ///   3. Verification must be done for **every privileged call**. The helper's root comes from launchd,
    ///      and has nothing to do with whether the user pressed the fingerprint -- it is root from the moment it starts, and any process able to connect to the Mach
    ///      service can send requests. So "who is calling" can only be answered by the helper itself.
    public static func verify(externalForm data: Data,
                              right: String = HelperConstants.privilegedRightName) throws {
        guard data.count == MemoryLayout<AuthorizationExternalForm>.size else {
            throw Failure.malformedCredential
        }

        var external = AuthorizationExternalForm()
        _ = withUnsafeMutableBytes(of: &external) { destination in
            data.copyBytes(to: destination.bindMemory(to: UInt8.self))
        }

        var authorization: AuthorizationRef?
        let restoreStatus = AuthorizationCreateFromExternalForm(&external, &authorization)
        guard restoreStatus == errAuthorizationSuccess, let authorization else {
            throw Failure.restoreFailed(restoreStatus)
        }
        // Note: **without** .destroyRights; see item 2 above.
        defer { AuthorizationFree(authorization, []) }

        var name = Array(right.utf8CString)
        let checkStatus: OSStatus = name.withUnsafeMutableBufferPointer { buffer in
            var item = AuthorizationItem(name: buffer.baseAddress!, valueLength: 0,
                                         value: nil, flags: 0)
            return withUnsafeMutablePointer(to: &item) { itemPointer in
                var rights = AuthorizationRights(count: 1, items: itemPointer)
                // Only .extendRights, **without** .interactionAllowed; see item 1 above.
                return AuthorizationCopyRights(authorization, &rights, nil, [.extendRights], nil)
            }
        }
        guard checkStatus == errAuthorizationSuccess else {
            throw Failure.rightNotHeld(checkStatus)
        }
    }
}
