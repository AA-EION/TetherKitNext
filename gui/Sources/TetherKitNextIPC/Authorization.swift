import Foundation
import Security

/// 授权凭据的取得（App 侧）与复核（helper 侧）。
///
/// ★ 两侧刻意放在同一个文件里 ★
///   它们的差别只有一个标志位（`.interactionAllowed`），而那一位放错就是一个
///   安全漏洞。写在一起，改一边时另一边就在眼前。
///
/// ★ 一句必须记住的话：授权 ≠ root ★
///   `AuthorizationCopyRights` 成功**不改变进程的任何东西** —— uid、euid 一个
///   都没动。它产出的只是一张「用户在某时刻通过密码/指纹确认过」的凭据。
///   权限本身必须另有来源（这里是 launchd 以 root 拉起的 helper）。
///
///   所以顺序不能反：不是「先弹指纹拿到 root，再去建网卡」，而是「先有常驻的
///   root helper，每次操作弹指纹拿凭据交给它复核」。

/// 一次授权的持有者。
///
/// ★ 它存在的唯一理由：外部形式**不是凭据本身，只是一个引用** ★
///
///   `AuthorizationMakeExternalForm` 产出的 32 字节里没有任何权利信息，它只是
///   指向 securityd 里那份授权的一把钥匙。只要 App 这边把 AuthorizationRef 释放
///   掉（尤其是带 `.destroyRights` 释放），securityd 里的东西就没了 ——
///   helper 随后 `AuthorizationCreateFromExternalForm` 会失败，
///   报 `errAuthorizationDenied (-60005)`。
///
///   所以 AuthorizationRef 必须**活到 XPC 往返结束之后**。用一个类来持有它，
///   让 ARC 去管这件事，比在每个调用点手写「记得最后再 free」可靠得多。
///
/// ★ 为什么敢标 @unchecked Sendable ★
///   HelperInstaller 要把令牌递到后台队列（AEWP 会阻塞在授权框上，不能占主
///   线程）。这里没有可变状态 —— 两个存储属性都是 let；AuthorizationRef 本身
///   按 Authorization Services 的文档是线程安全的（真正的状态在 securityd
///   进程里，跨进程调用天然串行化）；deinit 由 ARC 保证只跑一次。
public final class AuthorizationToken: @unchecked Sendable {
    /// 可以跨进程传给 helper 的 32 字节外部形式。
    public let externalForm: Data

    private let authorization: AuthorizationRef

    init(authorization: AuthorizationRef, externalForm: Data) {
        self.authorization = authorization
        self.externalForm = externalForm
    }

    /// 把底层的 AuthorizationRef 短暂借给需要它本体的 API（目前只有
    /// `AuthorizationExecuteWithPrivileges` 一处 —— 它要的是 ref，不是外部形式）。
    ///
    /// 做成作用域借用而不是直接暴露属性：ref 的生命周期归本类管，谁把它存到
    /// 闭包外面，令牌一释放就是悬垂引用 —— 那种错误编译器查不出来。
    public func withReference<T>(_ body: (AuthorizationRef) throws -> T) rethrows -> T {
        try body(authorization)
    }

    deinit {
        // 带 .destroyRights：凭据归 App 所有，用完就销毁，不在进程里留一张
        // 长期有效的通行证。（helper 那边复核时**不能**带这个标志，
        // 否则会把这边的授权一起作废 —— 见 AuthorizationVerifier。）
        AuthorizationFree(authorization, [.destroyRights])
    }
}

/// App 侧：向用户请求授权，拿到可以跨进程传递的凭据。
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

    /// 弹出系统授权框，成功后返回持有这次授权的令牌。
    ///
    /// ★ 令牌应当被**缓存复用**，而不是每次操作重新取一次 ★
    ///
    ///   `system.privilege.admin` 这条规则的实测参数是 `shared = false`、
    ///   `timeout = 300`：
    ///     * `shared = false` —— 凭据**不跨 AuthorizationRef 共享**。每次
    ///       `AuthorizationCreate` 建一个新 ref，就必然要用户重新认证一次。
    ///     * `timeout = 300` —— 但在**同一个 ref** 上，凭据 5 分钟内有效。
    ///
    ///   所以「连接、配网络、断开」各弹一次框，纯粹是因为我们每次都新建 ref。
    ///   复用同一个令牌，5 分钟内只需要认证一次。这也是 Apple 自己在
    ///   EvenBetterAuthorizationSample 里的做法。
    ///
    ///   过期之后 helper 侧的复核会失败，调用方据此丢弃缓存、重新取一次即可。
    ///
    /// ⚠️ **调用方必须让令牌活到 XPC 往返结束之后。** 令牌一释放，securityd 里的
    /// 授权就没了，helper 还原外部形式时会报 `errAuthorizationDenied (-60005)`。
    ///
    /// - Parameter prompt: 显示在系统授权框里的一句说明，告诉用户这次授权是干
    ///   什么用的。不传的话框里只有干巴巴的「想要进行更改」。
    ///
    /// 必须在主线程调用 —— 它会呈现 UI。
    public static func requestAuthorization(
        right: String = HelperConstants.privilegedRightName,
        prompt: String? = nil
    ) throws -> AuthorizationToken {
        var authorization: AuthorizationRef?
        let createStatus = AuthorizationCreate(nil, nil, [], &authorization)
        guard createStatus == errAuthorizationSuccess, let authorization else {
            throw Failure.internalFailure(createStatus)
        }
        // 失败路径上要立刻释放；成功路径上所有权交给 AuthorizationToken。
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

    /// 请求权利，必要时弹框。
    ///
    /// 单独抽出来是因为环境项的构造要嵌好几层 `withUnsafe*` —— 那些缓冲必须活
    /// 到 `AuthorizationCopyRights` 返回之后，写在主流程里很容易被后来的人
    /// 「顺手整理」成悬垂指针。
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

                // App 侧**要**带 .interactionAllowed —— 弹框正是我们要的。
                // .preAuthorize 让权利当场就被授予，而不是等到真正使用时，
                // 这样凭据才会进入这个 ref 的缓存、供后续操作复用。
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

/// helper 侧：复核调用方递过来的凭据。
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

    /// 校验凭据里确实已经包含指定权利。不包含就抛错。
    ///
    /// ★ 三条必须照做的细节 ★
    ///
    ///   1. **绝不能带 `.interactionAllowed`。** daemon 没有 UI 会话；真让它能
    ///      弹框，等于任何能连上 Mach 服务的进程都能随意触发系统授权弹框骚扰
    ///      用户。这一步只查「这份凭据里已经有这项权利了吗」，不获取新权利。
    ///
    ///   2. **`AuthorizationFree` 不能带 `.destroyRights`。** 凭据归 App 所有，
    ///      helper 只是借来核对；带上会把 App 那边的授权一起作废。
    ///
    ///   3. 复核是**每次特权调用**都要做的。helper 的 root 来自 launchd，
    ///      跟用户按没按指纹毫无关系 —— 它一启动就是 root，任何能连上 Mach
    ///      服务的进程都能发请求。所以「谁在调用」只能由 helper 自己回答。
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
        // 注意：**不带** .destroyRights，见上面第 2 条。
        defer { AuthorizationFree(authorization, []) }

        var name = Array(right.utf8CString)
        let checkStatus: OSStatus = name.withUnsafeMutableBufferPointer { buffer in
            var item = AuthorizationItem(name: buffer.baseAddress!, valueLength: 0,
                                         value: nil, flags: 0)
            return withUnsafeMutablePointer(to: &item) { itemPointer in
                var rights = AuthorizationRights(count: 1, items: itemPointer)
                // 只有 .extendRights，**没有** .interactionAllowed，见上面第 1 条。
                return AuthorizationCopyRights(authorization, &rights, nil, [.extendRights], nil)
            }
        }
        guard checkStatus == errAuthorizationSuccess else {
            throw Failure.rightNotHeld(checkStatus)
        }
    }
}
