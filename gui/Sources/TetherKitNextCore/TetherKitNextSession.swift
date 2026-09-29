import CTetherKitNext
import Foundation
import TetherKitNextIPC

/// One RNDIS session. **Needs root**, so it is used only inside the helper process.
///
/// Thread safety: the C-side session itself is thread-safe (both the state snapshot and the event queue have locks), but
/// the calling order of `start` / `stop` must be guaranteed non-concurrent by the caller. The helper strings all lifecycle calls
/// together on one serial queue to satisfy this.
/// `@unchecked Sendable`: the only state is the C handle, and the C ABI
/// documents every tk_session_* call as thread-safe (status snapshots are
/// taken under the runtime's own lock). The helper touches a session from its
/// lifecycle queue and from XPC status polls concurrently.
public final class TetherKitNextSession: @unchecked Sendable {
    private let handle: OpaquePointer

    /// The maximum number of events taken away at once, consistent with the C-side ring buffer capacity (128).
    private static let eventCapacity = 128

    public init(configuration: SessionConfiguration) throws {
        var raw = tk_session_config_t()
        // It must be init first and then modified: the struct has a pile of tuning knobs, and all-zero initialization would give an invalid configuration.
        tk_session_config_init(&raw)
        raw.vendor_id = configuration.vendorID
        raw.product_id = configuration.productID
        raw.bus_number = configuration.busNumber
        raw.device_address = configuration.deviceAddress
        raw.mtu = configuration.mtu
        raw.adopt_device_mac = configuration.adoptDeviceMAC

        var error = tk_error_t()
        guard let created = tk_session_create(&raw, &error) else {
            throw TetherKitNextError(result: TK_ERR_FAILED.rawValue, error: error)
        }
        handle = created
    }

    deinit {
        // tk_session_destroy stops first internally (idempotent), so there is no need to stop first here.
        tk_session_destroy(handle)
    }

    /// Starts. **Non-blocking** -- returning only means the request was accepted; success or failure must be checked via the subsequent status.
    public func start() throws {
        var error = tk_error_t()
        let result = tk_session_start(handle, &error)
        try check(result, error)
    }

    /// Stops and waits for all teardown to complete. Idempotent.
    public func stop() {
        _ = tk_session_stop(handle)
    }

    /// Takes a state snapshot. Callable from any thread.
    public func status() -> SessionStatus {
        var raw = tk_session_status_t()
        guard tk_session_status_get(handle, &raw) == TK_OK else {
            return .idle
        }
        return SessionStatus(
            runState: RunState(rawValue: raw.run_state) ?? .idle,
            rndisState: RndisState(rawValue: raw.rndis_state) ?? .uninitialized,
            linkUp: raw.link_up,
            paused: raw.paused,
            systemInterface: String(fixedCArray: raw.system_interface),
            driverInterface: String(fixedCArray: raw.driver_interface),
            deviceMAC: formatMAC(raw.device_mac),
            mtu: raw.mtu,
            linkSpeedMbps: raw.link_speed_mbps,
            vendorDescription: String(fixedCArray: raw.vendor_description),
            deviceDescription: String(fixedCArray: raw.device_description),
            rxFrames: raw.rx_frames,
            rxBytes: raw.rx_bytes,
            rxDropped: raw.rx_dropped,
            txFrames: raw.tx_frames,
            txBytes: raw.tx_bytes,
            txDropped: raw.tx_dropped,
            rxQueueDepth: raw.rx_queue_depth,
            linkKernelDrops: raw.link_kernel_drops,
            txBackpressure: raw.tx_backpressure,
            monotonicNanos: raw.monotonic_nanos,
            fatalMessage: String(fixedCArray: raw.fatal))
    }

    /// Takes away the queued events, rendering them into one sentence for the user to read.
    ///
    /// Keeps only the categories that "users care about": link changes, device resets, fatal errors. The RNDIS internal
    /// state transitions and lifecycle transitions are already shown by the UI via status, and flooding them again as notifications would be just noise.
    public func drainNotices() -> [String] {
        var buffer = [tk_event_t](repeating: tk_event_t(), count: Self.eventCapacity)
        let taken = tk_session_poll_events(handle, &buffer, Self.eventCapacity)

        return buffer.prefix(taken).compactMap { event -> String? in
            // The C side's tk_event_kind values are all non-negative and are imported by Swift as UInt32, while the struct
            // field is int32_t -- the two cannot be compared directly and must be converted explicitly once.
            switch event.kind {
            case Int32(TK_EVENT_LINK.rawValue):
                return L(event.a == 1 ? .eventLinkUp : .eventLinkDown)
            case Int32(TK_EVENT_DEVICE_RESET.rawValue):
                return L(event.a == 1 ? .eventDeviceResetReplayed : .eventDeviceReset)
            case Int32(TK_EVENT_FATAL.rawValue):
                return String(fixedCArray: event.text)
            case Int32(TK_EVENT_NEGOTIATED.rawValue):
                return L(.eventNegotiated, Int(event.a), Int(event.b))
            default:
                return nil
            }
        }
    }
}
