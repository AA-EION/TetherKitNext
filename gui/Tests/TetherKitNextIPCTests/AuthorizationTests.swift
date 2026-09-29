import Security
import XCTest

@testable import TetherKitNextIPC

/// Regression tests of the authorization credential lifecycle.
///
/// * What is pinned down here is a pitfall that was really hit *
///   The 32 bytes produced by `AuthorizationMakeExternalForm` are **not the credential itself**, only
///   a key pointing to that authorization in securityd. Once the App side releases the AuthorizationRef early
///   (especially with `.destroyRights`), the helper's restore fails -- and the failure information is just one
///   `-60005`, from which it is completely impossible to tell from the code that "it was destroyed early by ourselves".
///
///   These cases **do not pop up an authorization dialog**: they only call `AuthorizationCreate` (creating an empty authorization session),
///   and do not call `AuthorizationCopyRights`, so no user interaction is needed and they can go into CI.
final class AuthorizationTests: XCTestCase {
    /// Externalizes an AuthorizationRef.
    private func externalForm(of authorization: AuthorizationRef) throws -> Data {
        var external = AuthorizationExternalForm()
        let status = AuthorizationMakeExternalForm(authorization, &external)
        try XCTSkipUnless(status == errAuthorizationSuccess,
                          "AuthorizationMakeExternalForm 失败（\(status)），跳过")
        return withUnsafeBytes(of: &external) { Data($0) }
    }

    /// Simulates the restore on the helper side, returning the status code.
    private func restore(_ data: Data) -> OSStatus {
        var external = AuthorizationExternalForm()
        _ = withUnsafeMutableBytes(of: &external) { destination in
            data.copyBytes(to: destination.bindMemory(to: UInt8.self))
        }
        var restored: AuthorizationRef?
        let status = AuthorizationCreateFromExternalForm(&external, &restored)
        if let restored {
            AuthorizationFree(restored, [])
        }
        return status
    }

    /// While the token is alive, the other side can restore it. This is the normal path.
    func testExternalFormRestorableWhileTokenAlive() throws {
        var authorization: AuthorizationRef?
        XCTAssertEqual(AuthorizationCreate(nil, nil, [], &authorization), errAuthorizationSuccess)
        let reference = try XCTUnwrap(authorization)

        let token = AuthorizationToken(authorization: reference,
                                       externalForm: try externalForm(of: reference))

        XCTAssertEqual(restore(token.externalForm), errAuthorizationSuccess,
                       "令牌还活着的时候，helper 侧必须能还原出凭据")

        withExtendedLifetime(token) {}
    }

    /// After the token is released it can no longer be restored -- this is exactly the cause of -60005 back then.
    ///
    /// Conversely: once this case turns red, it means "releasing early is fine", and then the whole AuthorizationToken
    /// layer has no reason to exist, and you should first find out what changed in system behavior before touching it.
    func testExternalFormUnusableAfterTokenReleased() throws {
        var authorization: AuthorizationRef?
        XCTAssertEqual(AuthorizationCreate(nil, nil, [], &authorization), errAuthorizationSuccess)
        let reference = try XCTUnwrap(authorization)

        let capturedForm: Data
        do {
            let token = AuthorizationToken(authorization: reference,
                                           externalForm: try externalForm(of: reference))
            capturedForm = token.externalForm
        }  // token is destroyed here, together with the rights

        XCTAssertEqual(restore(capturedForm), errAuthorizationDenied,
                       "令牌释放后外部形式必须失效；当初就是它导致了 -60005")
    }
}
