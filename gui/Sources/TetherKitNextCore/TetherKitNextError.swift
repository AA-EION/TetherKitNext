import CTetherKitNext
import TetherKitNextIPC
import Foundation

/// 来自 C ABI 的错误。
///
/// `message` 已经是可以直接展示给用户的中文（库那边就是按这个标准写的），
/// 界面不需要再翻译一次。
public struct TetherKitNextError: LocalizedError, Sendable {
    public enum Domain: Int32, Sendable {
        case generic = 0
        case errno = 1
        case libusb = 2
        case rndis = 3
    }

    public let result: Int32
    public let domain: Domain
    public let code: Int64
    public let message: String

    public var errorDescription: String? { message }

    /// 是否是「缺少权限」—— 界面据此决定弹授权还是报错。
    public var isPermissionDenied: Bool { result == TK_ERR_PERMISSION.rawValue }

    init(result: Int32, error: tk_error_t) {
        self.result = result
        self.domain = Domain(rawValue: error.domain) ?? .generic
        self.code = error.code
        let text = String(fixedCArray: error.message)
        // C 侧在参数校验失败的分支上未必填了消息（比如纯空指针检查），
        // 这时给一句兜底的，总比界面上出现一个空的错误框强。
        self.message = text.isEmpty ? L(.libraryGenericFailure, Int(result)) : text
    }

    public init(message: String) {
        self.result = TK_ERR_FAILED.rawValue
        self.domain = .generic
        self.code = 0
        self.message = message
    }
}

/// 把 C 的返回码 + 错误结构体翻译成 Swift 的抛出。
func check(_ result: tk_result_t, _ error: tk_error_t) throws {
    guard result != TK_OK else { return }
    throw TetherKitNextError(result: result.rawValue, error: error)
}
