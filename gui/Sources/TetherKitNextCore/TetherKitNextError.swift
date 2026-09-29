import CTetherKitNext
import TetherKitNextIPC
import Foundation

/// An error from the C ABI.
///
/// `message` is already text that can be shown directly to the user (the library was written to this standard),
/// so the UI need not translate it again.
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

    /// Whether it is "missing permission" -- the UI uses it to decide whether to pop up authorization or report an error.
    public var isPermissionDenied: Bool { result == TK_ERR_PERMISSION.rawValue }

    init(result: Int32, error: tk_error_t) {
        self.result = result
        self.domain = Domain(rawValue: error.domain) ?? .generic
        self.code = error.code
        let text = String(fixedCArray: error.message)
        // On the argument-validation failure branches the C side does not necessarily fill in a message (such as a plain null-pointer check),
        // and in that case giving a fallback sentence is better than showing an empty error box on the UI.
        self.message = text.isEmpty ? L(.libraryGenericFailure, Int(result)) : text
    }

    public init(message: String) {
        self.result = TK_ERR_FAILED.rawValue
        self.domain = .generic
        self.code = 0
        self.message = message
    }
}

/// Translates C's return code + error struct into a Swift throw.
func check(_ result: tk_result_t, _ error: tk_error_t) throws {
    guard result != TK_OK else { return }
    throw TetherKitNextError(result: result.rawValue, error: error)
}
