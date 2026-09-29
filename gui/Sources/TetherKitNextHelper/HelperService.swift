import Foundation
import TetherKitNextCore
import TetherKitNextIPC

/// The helper's XPC service implementation.
///
/// * Threading model *
///
///   XPC methods are invoked on the connection's own queue. Any potentially time-consuming operation (starting a session does a USB
///   handshake, DHCP waits for a lease) is **immediately dispatched to our own serial queue with an asynchronous reply**, and must never
///   block on the XPC queue -- otherwise subsequent status polls on the same connection get stuck too, and the UI shows up
///   as "everything frozen".
///
///   The session lifecycle and NIC configuration each use one serial queue: the two do not block each other, but each is strictly serial internally
///   (concurrent start/stop or concurrent ipconfig would be a disaster). State queries are not queued and go straight to
///   the C layer -- the snapshot over there is thread-safe anyway.
///
/// `@unchecked Sendable`: all mutable state is guarded by `stateLock`, and the
/// work queues are serial. XPC invokes methods on arbitrary threads.
final class HelperService: NSObject, TetherKitNextHelperProtocol, @unchecked Sendable {
    private let lifecycleQueue = DispatchQueue(label: "com.tetherkitnext.helper.lifecycle")
    private let networkQueue = DispatchQueue(label: "com.tetherkitnext.helper.network")

    /// Protects session and notices. Hold times are all extremely short, so an ordinary lock suffices.
    private let stateLock = NSLock()
    private var session: TetherKitNextSession?
    /// startSession is in the middle of constructing / handshaking. During this window session is not yet registered, yet the device
    /// **has already** been opened by this process -- the occupancy judgment for enumeration must count it, otherwise string
    /// reads would insert one more transfer onto the control endpoint that is mid-handshake.
    private var sessionStarting = false
    /// Hints not yet taken away by the App.
    private var pendingNotices: [String] = []

    // MARK: - Probe interfaces that need no authorization

    func helperVersion(reply: @escaping @Sendable (String) -> Void) {
        // Include the XPC interface revision number, so the App can detect "the helper is an old pre-upgrade version".
        let info = TetherKitNextLibrary.versionInfo
        reply(HelperConstants.encodeVersion(info.version, build: info.build))
    }

    func environment(reply: @escaping @Sendable (Data?, String?) -> Void) {
        respond(with: TetherKitNextLibrary.checkEnvironment(), reply: reply)
    }

    func listDevices(reply: @escaping @Sendable (Data?, String?) -> Void) {
        // While the device is held by this process, do not read string descriptors: reading needs libusb_open, which fails in vain,
        // and would also insert a transfer onto the control endpoint that is mid-handshake. The judgment uses "really holding" rather than "session
        // is non-nil" -- dead sessions that are failed / stopped have long since torn USB down cleanly, and counting them as
        // "occupied" would make the name unreadable forever after the device is unplugged and replugged.
        // Skipping does not lose names: the C layer backfills the value last read successfully (see environment.cc).
        let deviceHeld = stateLock.withLock { () -> Bool in
            if sessionStarting { return true }
            guard let session else { return false }
            switch session.status().runState {
            case .starting, .running, .stopping: return true
            case .idle, .stopped, .failed: return false
            }
        }
        do {
            let devices = try TetherKitNextLibrary.listDevices(readStrings: !deviceHeld)
            respond(with: devices, reply: reply)
        } catch {
            reply(nil, error.localizedDescription)
        }
    }

    func sessionStatus(reply: @escaping @Sendable (Data?, String?) -> Void) {
        let status = withState { $0?.status() } ?? .idle
        // Also collect session events into the hint queue -- status polling happens once per refresh period anyway,
        // and riding along saves one XPC round trip.
        if let notices = withState({ $0?.drainNotices() }), !notices.isEmpty {
            appendNotices(notices)
        }
        respond(with: status, reply: reply)
    }

    func queryNetwork(interface: String, reply: @escaping @Sendable (Data?, String?) -> Void) {
        do {
            respond(with: try NetworkConfigurator.query(interface: interface), reply: reply)
        } catch {
            reply(nil, error.localizedDescription)
        }
    }

    func drainFeed(reply: @escaping @Sendable (Data?) -> Void) {
        let drained = TetherKitNextLibrary.drainLogs()
        let notices = takeNotices()
        let feed = HelperFeed(logs: drained.entries, droppedLogs: drained.dropped, notices: notices)
        reply(try? JSONEncoder().encode(feed))
    }

    func setLanguage(_ rawValue: String, reply: @escaping @Sendable () -> Void) {
        // If it is not recognized, leave things as they are. Better to keep using the previous language than to fall back to the default because the App passed a new value
        // -- that would show up as "switched language, and the helper's logs turned back to English instead".
        if let language = Language(rawValue: rawValue) {
            L10n.apply(language == .chinese ? .chinese : .english)
            TetherKitNextLibrary.setLanguage(language)
        }
        reply()
    }

    // MARK: - Privileged interfaces that need authorization

    func startSession(authorization: Data, configuration: Data,
                      reply: @escaping @Sendable (String?, Bool) -> Void) {
        guard let configuration = decode(SessionConfiguration.self, from: configuration, reply: reply),
              authorize(authorization, reply: reply) else {
            return
        }

        lifecycleQueue.async { [weak self] in
            guard let self else { return }

            // An old session that is completely dead (failed / stopped) must not block a new connection.
            //
            // Typical scenario: the device is unplugged -> consecutive keepalive failures -> the session enters failed. The C++ side
            // has already torn down USB and the NIC entirely by then, and the Swift layer is left with only a shell -- but it is
            // non-nil. If we only rejected on "session != nil", after the user replugs the device it could never
            // connect again, and only reinstalling the helper would help. This pitfall was really hit.
            if let existing = self.withState({ $0 }) {
                let state = existing.status().runState
                guard state == .failed || state == .stopped else {
                    reply(L(.helperSessionAlreadyRunning), false)
                    return
                }
                existing.stop()  // idempotent, just insurance
                self.stateLock.withLock { self.session = nil }
            }

            // Expose "the device is already held" throughout the whole handshake period from construction to start() returning,
            // so a concurrently arriving listDevices skips string reads (see the explanation over there).
            self.stateLock.withLock { self.sessionStarting = true }
            defer { self.stateLock.withLock { self.sessionStarting = false } }
            do {
                let session = try TetherKitNextSession(configuration: configuration)
                try session.start()
                self.stateLock.withLock { self.session = session }
                reply(nil, false)
            } catch {
                reply(error.localizedDescription, false)
            }
        }
    }

    func stopSession(authorization: Data, reply: @escaping @Sendable (String?, Bool) -> Void) {
        guard authorize(authorization, reply: reply) else { return }

        lifecycleQueue.async { [weak self] in
            guard let self else { return }
            // First take the session out of the state and then stop it: stopping has to join the control thread and may take
            // hundreds of milliseconds, during which it should not keep claiming "the session is running" externally.
            let session = self.stateLock.withLock { () -> TetherKitNextSession? in
                defer { self.session = nil }
                return self.session
            }
            session?.stop()
            self.appendNotices([L(.helperSessionStopped)])
            reply(nil, false)
        }
    }

    func applyNetwork(authorization: Data, interface: String, configuration: Data,
                      reply: @escaping @Sendable (String?, Bool) -> Void) {
        guard let configuration = decode(NetworkConfiguration.self, from: configuration, reply: reply),
              authorize(authorization, reply: reply) else {
            return
        }
        // The UI has already validated once, but XPC is reachable by any local process, and the helper must put up its own barrier
        // -- and it uses the same rules, so the two sides never judge inconsistently.
        if let message = NetworkValidator.validationMessage(for: configuration) {
            reply(message, false)
            return
        }

        // DHCP blocks until a lease is obtained (the library's internal cap is 10 seconds), so it must reply asynchronously.
        networkQueue.async { [weak self] in
            do {
                try NetworkConfigurator.apply(configuration, to: interface)
                self?.appendNotices([L(.helperNetworkApplied, configuration.mode.displayName)])
                reply(nil, false)
            } catch {
                reply(error.localizedDescription, false)
            }
        }
    }

    func setCommandLineToolInstalled(authorization: Data, install: Bool,
                                     reply: @escaping @Sendable (String?, Bool) -> Void) {
        guard authorize(authorization, reply: reply) else { return }
        networkQueue.async {
            do {
                if install {
                    try CommandLineToolLink.install()
                } else {
                    try CommandLineToolLink.uninstall()
                }
                reply(nil, false)
            } catch {
                reply(error.localizedDescription, false)
            }
        }
    }

    // MARK: - Shutdown cleanup

    /// Called on receiving SIGTERM (`launchctl bootout`).
    ///
    /// Swift's deinit does not run when the process is terminated, and without actively stopping, the feth NIC would leak in the
    /// kernel. The on-disk registration can catch SIGKILL, but when a graceful exit is possible it should still exit gracefully --
    /// that way even the "clean up at next launch" step is saved.
    func shutdown() {
        let session = stateLock.withLock { () -> TetherKitNextSession? in
            defer { self.session = nil }
            return self.session
        }
        session?.stop()
    }

    // MARK: - Internal utilities

    /// Verifies authorization; when it does not pass, replies with an error and sets the second parameter to true, telling the App
    /// "this is an authorization problem, popping up the dialog again and trying once more may succeed".
    ///
    /// ★ Empty authorization (Touch ID path, protocol revision 5) ★
    ///
    ///   A team-signed app confirms the user with LocalAuthentication (Touch
    ///   ID, Apple Watch or the login password) and then sends no admin
    ///   authorization at all. That is only acceptable when both hold:
    ///     * this daemon pinned the connection to TetherKitNext.app signed by
    ///       our own team (`CodeSigning.clientRequirement`), so the caller is
    ///       our app and not some other local process skipping the prompt;
    ///     * the calling user is an administrator — the same population the
    ///       `system.privilege.admin` dialog accepts, so the policy does not
    ///       widen, only the way an admin proves presence changes.
    ///   Anything else is rejected as an authorization problem, and the app
    ///   falls back to the administrator dialog.
    ///
    ///   Must be called synchronously from the XPC method, where
    ///   `NSXPCConnection.current()` is the caller's connection.
    private func authorize(_ data: Data, reply: @escaping @Sendable (String?, Bool) -> Void) -> Bool {
        if data.isEmpty {
            guard CodeSigning.clientRequirement != nil,
                  let connection = NSXPCConnection.current(),
                  AdminGroup.contains(uid: connection.effectiveUserIdentifier) else {
                reply(L(.helperPresenceNotAccepted), true)
                return false
            }
            return true
        }
        do {
            try AuthorizationVerifier.verify(externalForm: data)
            return true
        } catch {
            reply(error.localizedDescription, true)
            return false
        }
    }

    private func decode<T: Decodable>(_ type: T.Type, from data: Data,
                                      reply: @escaping @Sendable (String?, Bool) -> Void) -> T? {
        do {
            return try JSONDecoder().decode(type, from: data)
        } catch {
            reply(L(.helperRequestDecodeFailed, error.localizedDescription), false)
            return nil
        }
    }

    private func respond<T: Encodable>(with value: T, reply: (Data?, String?) -> Void) {
        do {
            reply(try JSONEncoder().encode(value), nil)
        } catch {
            reply(nil, L(.helperReplyEncodeFailed, error.localizedDescription))
        }
    }

    private func withState<T>(_ body: (TetherKitNextSession?) -> T) -> T {
        stateLock.withLock { body(session) }
    }

    private func appendNotices(_ notices: [String]) {
        guard !notices.isEmpty else { return }
        stateLock.withLock {
            pendingNotices.append(contentsOf: notices)
            // Cap of 200: if the App does not come to take them for a long time (say it is suspended), it should not be allowed to grow without bound.
            if pendingNotices.count > 200 {
                pendingNotices.removeFirst(pendingNotices.count - 200)
            }
        }
    }

    private func takeNotices() -> [String] {
        stateLock.withLock {
            defer { pendingNotices.removeAll() }
            return pendingNotices
        }
    }
}

/// XPC listener delegate.
final class HelperListenerDelegate: NSObject, NSXPCListenerDelegate {
    private let service: HelperService

    init(service: HelperService) {
        self.service = service
    }

    func listener(_ listener: NSXPCListener,
                  shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        // ★ Who may connect ★
        //
        //   Team-signed (Developer ID) builds only accept TetherKitNext.app signed by
        //   the same team; the check runs against the peer's audit token for
        //   every message. Ad-hoc development builds have no Team ID to pin, so
        //   they fall back to the original model: anyone may connect, but every
        //   privileged call must carry an admin authorization that is re-verified
        //   here (AuthorizationVerifier). Release builds enforce both layers.
        if let requirement = CodeSigning.clientRequirement {
            connection.setCodeSigningRequirement(requirement)
        }
        connection.exportedInterface = NSXPCInterface(with: TetherKitNextHelperProtocol.self)
        connection.exportedObject = service
        connection.resume()
        return true
    }
}
