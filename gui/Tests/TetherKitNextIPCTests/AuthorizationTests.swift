import Security
import XCTest

@testable import TetherKitNextIPC

/// 授权凭据生命周期的回归测试。
///
/// ★ 这里钉住的是一个真实踩过的坑 ★
///   `AuthorizationMakeExternalForm` 产出的 32 字节**不是凭据本身**，只是指向
///   securityd 里那份授权的一把钥匙。App 侧一旦提前把 AuthorizationRef 释放
///   （尤其是带 `.destroyRights`），helper 还原时就会失败 —— 而失败信息只有一个
///   `-60005`，从代码上完全看不出「是被自己提前销毁的」。
///
///   这些用例**不会弹授权框**：只调 `AuthorizationCreate`（建立空的授权会话），
///   不调 `AuthorizationCopyRights`，所以不需要任何用户交互，可以放进 CI。
final class AuthorizationTests: XCTestCase {
    /// 把 AuthorizationRef 外部化。
    private func externalForm(of authorization: AuthorizationRef) throws -> Data {
        var external = AuthorizationExternalForm()
        let status = AuthorizationMakeExternalForm(authorization, &external)
        try XCTSkipUnless(status == errAuthorizationSuccess,
                          "AuthorizationMakeExternalForm 失败（\(status)），跳过")
        return withUnsafeBytes(of: &external) { Data($0) }
    }

    /// 模拟 helper 侧的还原，返回状态码。
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

    /// 令牌活着时，另一侧能还原出来。这是正常路径。
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

    /// 令牌被释放后就还原不出来了 —— 这正是当初 -60005 的成因。
    ///
    /// 反过来说：这条用例一旦变红，说明「提前释放也没事」，那 AuthorizationToken
    /// 这一整层就没有存在意义了，应该先搞清楚系统行为变了什么再动它。
    func testExternalFormUnusableAfterTokenReleased() throws {
        var authorization: AuthorizationRef?
        XCTAssertEqual(AuthorizationCreate(nil, nil, [], &authorization), errAuthorizationSuccess)
        let reference = try XCTUnwrap(authorization)

        let capturedForm: Data
        do {
            let token = AuthorizationToken(authorization: reference,
                                           externalForm: try externalForm(of: reference))
            capturedForm = token.externalForm
        }  // token 在这里析构，连同权利一起销毁

        XCTAssertEqual(restore(capturedForm), errAuthorizationDenied,
                       "令牌释放后外部形式必须失效；当初就是它导致了 -60005")
    }
}
