import Foundation
import TetherKitNextIPC

/// The client that communicates with tetherkitnext-helper.
///
/// Wraps "reply-block based XPC" into async/await. It looks trivial, but there is one place that needs care:
/// NSXPCConnection's error-handling block and the method's reply block **may both be called** (for example the request has already been
/// sent and the connection then drops), and a CheckedContinuation resumed twice is a direct crash.
/// So every call is wrapped in a ContinuationGuard, guaranteeing it is fulfilled only once.
/// `@unchecked Sendable`: the cached connection is guarded by `connectionLock`;
/// everything else is immutable. AppModel (main actor) awaits these calls
/// while XPC replies arrive on XPC's own queues.
final class HelperClient: @unchecked Sendable {
    enum Failure: LocalizedError {
        /// Cannot connect -- the most common reason is that the helper is not installed yet.
        case unreachable(String)
        /// The helper explicitly returned an error.
        case helper(String)
        /// The helper's authorization verification did not pass -- most likely the credential expired, and popping up the dialog again is enough.
        case authorizationRejected(String)
        /// The reply cannot be decoded (the versions on the two ends are inconsistent).
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

        /// Whether it is worth "fetching the authorization again and retrying".
        var isAuthorizationProblem: Bool {
            if case .authorizationRejected = self { return true }
            return false
        }
    }

    /// Guarantees a continuation is fulfilled only once.
    ///
    /// It is possible for XPC's error block and reply block to both be called, and resuming twice crashes --
    /// this pitfall appears only in timing like "the connection drops after the request is sent", and is hard to hit through tests.
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

    /// Gets (creating if necessary) the connection to the helper.
    private func connection() -> NSXPCConnection {
        connectionLock.withLock {
            if let existing = cachedConnection {
                return existing
            }
            let created = NSXPCConnection(machServiceName: HelperConstants.machServiceName,
                                          options: .privileged)
            created.remoteObjectInterface = NSXPCInterface(with: TetherKitNextHelperProtocol.self)
            // Release builds only talk to the daemon signed by our own team, so
            // a look-alike service registered under our name is refused.
            if let requirement = CodeSigning.daemonRequirement {
                created.setCodeSigningRequirement(requirement)
            }

            // After the connection drops the cache must be discarded, otherwise later calls would keep hitting a dead connection,
            // showing up as "the helper is clearly installed but can never be connected to".
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

    /// Makes one call. `body` gets the proxy and the guard, and is responsible for issuing the request and fulfilling the result.
    private func invoke<T: Sendable>(
        _ body: @escaping (TetherKitNextHelperProtocol, ContinuationGuard<T>) -> Void
    ) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            let guarded = ContinuationGuard(continuation)
            let proxy = connection().remoteObjectProxyWithErrorHandler { error in
                guarded.resume(throwing: Failure.unreachable(error.localizedDescription))
            }
            guard let typed = proxy as? TetherKitNextHelperProtocol else {
                guarded.resume(throwing: Failure.malformedResponse)
                return
            }
            body(typed, guarded)
        }
    }

    /// The generic handling of replies of the kind "return a Codable or an error".
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

    // MARK: - Probing (no authorization needed)

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

    /// Tells the helper the UI language. **Failures are swallowed silently** -- the language failing to sync only means the log card
    /// mixes in another language, and it should not pop the "cannot connect to helper" error to the user (in the normal startup
    /// order the helper may not be installed yet).
    func setLanguage(_ language: Language) async {
        _ = try? await invoke { proxy, guarded in
            proxy.setLanguage(language.rawValue) { guarded.resume(returning: ()) }
        }
    }

    // MARK: - Privileged operations (authorization credential needed)

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
        _ body: @escaping (TetherKitNextHelperProtocol, ContinuationGuard<Void>) -> Void
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
