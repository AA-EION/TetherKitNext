import Foundation
import TetherKitIPC

/// 与 tetherkit-helper 通信的客户端。
///
/// 把「基于 reply block 的 XPC」包成 async/await。看着琐碎，但有一处必须小心：
/// NSXPCConnection 的错误处理块与方法的 reply 块**可能都被调用**（比如请求已
/// 发出、连接随后断开），而 CheckedContinuation 被 resume 两次是直接崩溃。
/// 因此每次调用都用 ContinuationGuard 包一层，保证只兑现一次。
/// `@unchecked Sendable`: the cached connection is guarded by `connectionLock`;
/// everything else is immutable. AppModel (main actor) awaits these calls
/// while XPC replies arrive on XPC's own queues.
final class HelperClient: @unchecked Sendable {
    enum Failure: LocalizedError {
        /// 连不上 —— 最常见的原因是 helper 还没安装。
        case unreachable(String)
        /// helper 明确返回了错误。
        case helper(String)
        /// helper 复核授权没通过 —— 多半是凭据过期了，重新弹框再来一次就行。
        case authorizationRejected(String)
        /// 应答解不出来（两端版本不一致）。
        case malformedResponse

        var errorDescription: String? {
            switch self {
            case .unreachable(let detail):
                return L(.helperConnectFailed, detail)
            case .helper(let message), .authorizationRejected(let message):
                return message
            case .malformedResponse:
                return L(.helperReplyUnparsable)
            }
        }

        var isUnreachable: Bool {
            if case .unreachable = self { return true }
            return false
        }

        /// 是否值得「重新取一次授权再试」。
        var isAuthorizationProblem: Bool {
            if case .authorizationRejected = self { return true }
            return false
        }
    }

    /// 保证一个 continuation 只被兑现一次。
    ///
    /// XPC 的错误块和 reply 块存在都被调用的可能，而重复 resume 会崩溃 ——
    /// 这个坑只在「请求发出后连接才断」这种时序上出现，很难靠测试撞到。
    private final class ContinuationGuard<T: Sendable>: @unchecked Sendable {
        private let lock = NSLock()
        private var continuation: CheckedContinuation<T, Error>?

        init(_ continuation: CheckedContinuation<T, Error>) {
            self.continuation = continuation
        }

        func resume(returning value: T) {
            take()?.resume(returning: value)
        }

        func resume(throwing error: Error) {
            take()?.resume(throwing: error)
        }

        private func take() -> CheckedContinuation<T, Error>? {
            lock.withLock {
                defer { continuation = nil }
                return continuation
            }
        }
    }

    private let connectionLock = NSLock()
    private var cachedConnection: NSXPCConnection?

    deinit {
        cachedConnection?.invalidate()
    }

    /// 取（必要时创建）到 helper 的连接。
    private func connection() -> NSXPCConnection {
        connectionLock.withLock {
            if let existing = cachedConnection {
                return existing
            }
            let created = NSXPCConnection(machServiceName: HelperConstants.machServiceName,
                                          options: .privileged)
            created.remoteObjectInterface = NSXPCInterface(with: TetherKitHelperProtocol.self)
            // Release builds only talk to the daemon signed by our own team, so
            // a look-alike service registered under our name is refused.
            if let requirement = CodeSigning.daemonRequirement {
                created.setCodeSigningRequirement(requirement)
            }

            // 连接断掉后必须丢弃缓存，否则后续调用会一直打在一条死连接上，
            // 表现为「helper 明明装好了却一直连不上」。
            let invalidate: @Sendable () -> Void = { [weak self] in
                self?.connectionLock.withLock { self?.cachedConnection = nil }
            }
            created.invalidationHandler = invalidate
            created.interruptionHandler = invalidate

            created.resume()
            cachedConnection = created
            return created
        }
    }

    /// 发一次调用。`body` 拿到代理与守卫，负责发起请求并兑现结果。
    private func invoke<T: Sendable>(
        _ body: @escaping (TetherKitHelperProtocol, ContinuationGuard<T>) -> Void
    ) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            let guarded = ContinuationGuard(continuation)
            let proxy = connection().remoteObjectProxyWithErrorHandler { error in
                guarded.resume(throwing: Failure.unreachable(error.localizedDescription))
            }
            guard let typed = proxy as? TetherKitHelperProtocol else {
                guarded.resume(throwing: Failure.malformedResponse)
                return
            }
            body(typed, guarded)
        }
    }

    /// 「返回一个 Codable 或一条错误」这种应答的通用处理。
    private func decode<T: Decodable & Sendable>(_ type: T.Type, data: Data?, message: String?,
                                      into guarded: ContinuationGuard<T>) {
        if let message {
            guarded.resume(throwing: Failure.helper(message))
            return
        }
        guard let data, let value = try? JSONDecoder().decode(type, from: data) else {
            guarded.resume(throwing: Failure.malformedResponse)
            return
        }
        guarded.resume(returning: value)
    }

    // MARK: - 探测（不需要授权）

    func helperVersion() async throws -> String {
        try await invoke { proxy, guarded in
            proxy.helperVersion { guarded.resume(returning: $0) }
        }
    }

    func environment() async throws -> EnvironmentReport {
        try await invoke { proxy, guarded in
            proxy.environment { data, message in
                self.decode(EnvironmentReport.self, data: data, message: message, into: guarded)
            }
        }
    }

    func listDevices() async throws -> [DeviceDescriptor] {
        try await invoke { proxy, guarded in
            proxy.listDevices { data, message in
                self.decode([DeviceDescriptor].self, data: data, message: message, into: guarded)
            }
        }
    }

    func sessionStatus() async throws -> SessionStatus {
        try await invoke { proxy, guarded in
            proxy.sessionStatus { data, message in
                self.decode(SessionStatus.self, data: data, message: message, into: guarded)
            }
        }
    }

    func queryNetwork(interface: String) async throws -> NetworkState {
        try await invoke { proxy, guarded in
            proxy.queryNetwork(interface: interface) { data, message in
                self.decode(NetworkState.self, data: data, message: message, into: guarded)
            }
        }
    }

    func drainFeed() async throws -> HelperFeed {
        try await invoke { proxy, guarded in
            proxy.drainFeed { data in
                guard let data, let feed = try? JSONDecoder().decode(HelperFeed.self, from: data)
                else {
                    guarded.resume(returning: .empty)
                    return
                }
                guarded.resume(returning: feed)
            }
        }
    }

    /// 把界面语言告诉 helper。**失败静默吞掉** —— 语言没同步上只是日志卡里
    /// 混了另一种语言，不该让它把「连不上 helper」的错误弹给用户（正常启动
    /// 顺序里 helper 可能还没装）。
    func setLanguage(_ language: Language) async {
        _ = try? await invoke { proxy, guarded in
            proxy.setLanguage(language.rawValue) { guarded.resume(returning: ()) }
        }
    }

    // MARK: - 特权操作（需要授权凭据）

    func startSession(authorization: Data, configuration: SessionConfiguration) async throws {
        let payload = try JSONEncoder().encode(configuration)
        try await invokeVoid { proxy, guarded in
            proxy.startSession(authorization: authorization,
                               configuration: payload) { message, authorizationFailed in
                Self.finish(message, authorizationFailed, guarded)
            }
        }
    }

    func stopSession(authorization: Data) async throws {
        try await invokeVoid { proxy, guarded in
            proxy.stopSession(authorization: authorization) { message, authorizationFailed in
                Self.finish(message, authorizationFailed, guarded)
            }
        }
    }

    func setCommandLineToolInstalled(authorization: Data, install: Bool) async throws {
        try await invokeVoid { proxy, guarded in
            proxy.setCommandLineToolInstalled(authorization: authorization,
                                              install: install) { message, authorizationFailed in
                Self.finish(message, authorizationFailed, guarded)
            }
        }
    }

    func applyNetwork(authorization: Data, interface: String,
                      configuration: NetworkConfiguration) async throws {
        let payload = try JSONEncoder().encode(configuration)
        try await invokeVoid { proxy, guarded in
            proxy.applyNetwork(authorization: authorization, interface: interface,
                               configuration: payload) { message, authorizationFailed in
                Self.finish(message, authorizationFailed, guarded)
            }
        }
    }

    private func invokeVoid(
        _ body: @escaping (TetherKitHelperProtocol, ContinuationGuard<Void>) -> Void
    ) async throws {
        _ = try await invoke(body)
    }

    private static func finish(_ message: String?, _ authorizationFailed: Bool,
                               _ guarded: ContinuationGuard<Void>) {
        guard let message else {
            guarded.resume(returning: ())
            return
        }
        guarded.resume(throwing: authorizationFailed
            ? Failure.authorizationRejected(message)
            : Failure.helper(message))
    }
}
