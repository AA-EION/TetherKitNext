import Foundation

// Data models transferred between the App and the helper.
//
// * Why everything goes through Codable + Data rather than NSSecureCoding classes *
//   XPC accepts only plist types or classes implementing NSSecureCoding, and the latter requires both ends to register the allowed
//   class set; getting one wrong is a runtime "disallowed class" exception, with an error message that is hard to pin down.
//   Directly encoding Codable structs into JSON Data and passing them over lets both ends share the same type definitions,
//   with validation left to JSONDecoder -- simple, and adding fields is naturally backward compatible.

// MARK: - Sessions

/// The startup configuration of an RNDIS session. The fields correspond one-to-one with the C ABI's tk_session_config_t.
public struct SessionConfiguration: Codable, Hashable, Sendable {
    /// Device filter. All 0 means use the first RNDIS device found.
    public var vendorID: UInt16
    public var productID: UInt16
    /// The bus number and device address must be given together, used to tell apart two devices of the same model.
    public var busNumber: UInt8
    public var deviceAddress: UInt8

    /// The desired MTU. Lowered by negotiation when the device cannot fit it.
    public var mtu: UInt32
    /// Whether to set the system-side NIC's MAC to the address the device reported.
    public var adoptDeviceMAC: Bool

    public init(vendorID: UInt16 = 0,
                productID: UInt16 = 0,
                busNumber: UInt8 = 0,
                deviceAddress: UInt8 = 0,
                mtu: UInt32 = 1500,
                adoptDeviceMAC: Bool = true) {
        self.vendorID = vendorID
        self.productID = productID
        self.busNumber = busNumber
        self.deviceAddress = deviceAddress
        self.mtu = mtu
        self.adoptDeviceMAC = adoptDeviceMAC
    }
}

/// Session lifecycle state. Raw values are aligned with C's tk_run_state_t.
public enum RunState: Int32, Codable, Sendable {
    case idle = 0
    case starting = 1
    case running = 2
    case stopping = 3
    case stopped = 4
    case failed = 5

    /// Whether it is in a "busy" transitional state -- the UI uses it to disable buttons and show progress.
    public var isTransitional: Bool { self == .starting || self == .stopping }
}

/// RNDIS host-side state. Raw values are aligned with C's tk_rndis_state_t.
public enum RndisState: Int32, Codable, Sendable {
    case uninitialized = 0
    case initializing = 1
    case initialized = 2
    /// Data starts flowing.
    case dataInitialized = 3
    case halting = 4
}

/// Session state snapshot.
public struct SessionStatus: Codable, Hashable, Sendable {
    public var runState: RunState
    public var rndisState: RndisState
    public var linkUp: Bool
    /// Whether data movement is paused (during link down or a device soft reset).
    public var paused: Bool

    /// System-side NIC name -- the user configures IP on this one.
    public var systemInterface: String
    /// Driver-side NIC name, for troubleshooting display only.
    public var driverInterface: String

    /// In the form "aa:bb:cc:dd:ee:ff"; empty when not negotiated.
    public var deviceMAC: String
    public var mtu: UInt32
    public var linkSpeedMbps: UInt32
    public var vendorDescription: String
    public var deviceDescription: String

    public var rxFrames: UInt64
    public var rxBytes: UInt64
    public var rxDropped: UInt64
    public var txFrames: UInt64
    public var txBytes: UInt64
    public var txDropped: UInt64
    public var rxQueueDepth: UInt64
    public var linkKernelDrops: UInt64
    public var txBackpressure: UInt64

    /// Monotonic nanosecond count at the sampling moment. **It must be used as the denominator when computing rates**, and the period of the UI timer
    /// must not be used -- the real interval between two pulls gets stretched by scheduling, and rates computed that way come out too high.
    public var monotonicNanos: Int64

    /// The cause when runState == .failed.
    public var fatalMessage: String

    /// The empty state when not connected.
    public static let idle = SessionStatus(
        runState: .idle, rndisState: .uninitialized, linkUp: false, paused: false,
        systemInterface: "", driverInterface: "", deviceMAC: "", mtu: 0, linkSpeedMbps: 0,
        vendorDescription: "", deviceDescription: "",
        rxFrames: 0, rxBytes: 0, rxDropped: 0, txFrames: 0, txBytes: 0, txDropped: 0,
        rxQueueDepth: 0, linkKernelDrops: 0, txBackpressure: 0,
        monotonicNanos: 0, fatalMessage: "")

    public init(runState: RunState, rndisState: RndisState, linkUp: Bool, paused: Bool,
                systemInterface: String, driverInterface: String, deviceMAC: String,
                mtu: UInt32, linkSpeedMbps: UInt32, vendorDescription: String,
                deviceDescription: String, rxFrames: UInt64, rxBytes: UInt64, rxDropped: UInt64,
                txFrames: UInt64, txBytes: UInt64, txDropped: UInt64, rxQueueDepth: UInt64,
                linkKernelDrops: UInt64, txBackpressure: UInt64, monotonicNanos: Int64,
                fatalMessage: String) {
        self.runState = runState
        self.rndisState = rndisState
        self.linkUp = linkUp
        self.paused = paused
        self.systemInterface = systemInterface
        self.driverInterface = driverInterface
        self.deviceMAC = deviceMAC
        self.mtu = mtu
        self.linkSpeedMbps = linkSpeedMbps
        self.vendorDescription = vendorDescription
        self.deviceDescription = deviceDescription
        self.rxFrames = rxFrames
        self.rxBytes = rxBytes
        self.rxDropped = rxDropped
        self.txFrames = txFrames
        self.txBytes = txBytes
        self.txDropped = txDropped
        self.rxQueueDepth = rxQueueDepth
        self.linkKernelDrops = linkKernelDrops
        self.txBackpressure = txBackpressure
        self.monotonicNanos = monotonicNanos
        self.fatalMessage = fatalMessage
    }
}

// MARK: - Devices

/// A USB device recognized as RNDIS.
public struct DeviceDescriptor: Codable, Hashable, Identifiable, Sendable {
    public var vendorID: UInt16
    public var productID: UInt16
    public var busNumber: UInt8
    public var deviceAddress: UInt8
    /// Vendor name / product name / serial number are best effort: reading them requires opening the device, and when the device is occupied
    /// they cannot be obtained -- but the helper backfills the value last read successfully, so the name stays
    /// stable before and after connecting. Only when even the backfill value is missing (never read since the helper started) is it an empty string, and the UI then
    /// falls back to showing `description`.
    public var manufacturer: String
    public var product: String
    public var serial: String
    /// In the form "Bus 020 Device 003: 18d1:4ee4", usable in any situation.
    public var summary: String
    public var usedAndroidQuirk: Bool

    /// Bus + address uniquely identify one connected device; two of the same model can also be told apart.
    public var id: String { "\(busNumber).\(deviceAddress)" }

    /// The primary title displayed in the UI.
    public var displayName: String {
        if !product.isEmpty {
            return manufacturer.isEmpty ? product : "\(manufacturer) \(product)"
        }
        return L(.usbDeviceFallbackName, Int(vendorID), Int(productID))
    }

    public init(vendorID: UInt16, productID: UInt16, busNumber: UInt8, deviceAddress: UInt8,
                manufacturer: String, product: String, serial: String, summary: String,
                usedAndroidQuirk: Bool) {
        self.vendorID = vendorID
        self.productID = productID
        self.busNumber = busNumber
        self.deviceAddress = deviceAddress
        self.manufacturer = manufacturer
        self.product = product
        self.serial = serial
        self.summary = summary
        self.usedAndroidQuirk = usedAndroidQuirk
    }
}

// MARK: - Network configuration

/// Connectivity method. Raw values are aligned with C's tk_ip_mode_t.
public enum IPMode: Int32, Codable, CaseIterable, Sendable {
    case dhcp = 0
    case manual = 1
    /// Revokes the configuration.
    case none = 2

    public var displayName: String {
        switch self {
        case .dhcp: return L(.ipModeDhcp)
        case .manual: return L(.ipModeManual)
        case .none: return L(.ipModeNone)
        }
    }
}

/// The connectivity method configuration of the NIC.
public struct NetworkConfiguration: Codable, Hashable, Sendable {
    public var mode: IPMode
    /// The following four items are used only when mode == .manual.
    public var address: String
    public var netmask: String
    public var router: String
    public var dnsServers: [String]
    /// Whether to point the **global** default route at this NIC as well.
    ///
    /// When off there is only one scoped default route bound to this interface. It only needs to be turned on when a higher-priority
    /// connected service (Wi-Fi, VPN) exists at the same time -- while the typical USB tethering scenario is precisely
    /// that no other network is available, in which case this NIC is naturally the primary service.
    public var setDefaultRoute: Bool

    public static let dhcp = NetworkConfiguration(mode: .dhcp)

    public init(mode: IPMode = .dhcp,
                address: String = "",
                netmask: String = "255.255.255.0",
                router: String = "",
                dnsServers: [String] = [],
                setDefaultRoute: Bool = false) {
        self.mode = mode
        self.address = address
        self.netmask = netmask
        self.router = router
        self.dnsServers = dnsServers
        self.setDefaultRoute = setDefaultRoute
    }
}

/// The state the NIC **actually has in effect** at present.
///
/// Deliberately does not restate "what we applied" but reads back the system: in static mode whether DNS takes effect depends on
/// whether IPMonitor accepts this service, and only a readback gives the user accurate feedback.
public struct NetworkState: Codable, Hashable, Sendable {
    public var hasAddress: Bool
    public var address: String
    public var netmask: String
    public var router: String
    public var dnsServers: [String]
    /// The configuration method reported by IPConfiguration ("DHCP" / "MANUAL" / "").
    public var method: String
    /// The service state reported by IPConfiguration (such as "BOUND").
    public var serviceState: String
    public var hasDefaultRoute: Bool
    /// Whether the global default route currently points at this NIC.
    public var isPrimaryDefaultRoute: Bool

    public static let empty = NetworkState(
        hasAddress: false, address: "", netmask: "", router: "", dnsServers: [],
        method: "", serviceState: "", hasDefaultRoute: false, isPrimaryDefaultRoute: false)

    public init(hasAddress: Bool, address: String, netmask: String, router: String,
                dnsServers: [String], method: String, serviceState: String,
                hasDefaultRoute: Bool, isPrimaryDefaultRoute: Bool) {
        self.hasAddress = hasAddress
        self.address = address
        self.netmask = netmask
        self.router = router
        self.dnsServers = dnsServers
        self.method = method
        self.serviceState = serviceState
        self.hasDefaultRoute = hasDefaultRoute
        self.isPrimaryDefaultRoute = isPrimaryDefaultRoute
    }
}

// MARK: - Logs and events

public enum LogLevel: Int32, Codable, Comparable, Sendable {
    case trace = 0
    case debug = 1
    case info = 2
    case warning = 3
    case error = 4

    public static func < (lhs: LogLevel, rhs: LogLevel) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    public var label: String {
        switch self {
        case .trace: return "TRACE"
        case .debug: return "DEBUG"
        case .info: return "INFO"
        case .warning: return "WARN"
        case .error: return "ERROR"
        }
    }
}

public struct LogEntry: Codable, Hashable, Identifiable, Sendable {
    public var id: UUID
    public var level: LogLevel
    public var timestamp: Date
    public var thread: String
    public var message: String

    public init(id: UUID = UUID(), level: LogLevel, timestamp: Date, thread: String,
                message: String) {
        self.id = id
        self.level = level
        self.timestamp = timestamp
        self.thread = thread
        self.message = message
    }
}

/// The reply to one helper "take away pending content".
///
/// Events and logs are merged into one call to press the number of XPC round trips down to one per refresh period --
/// the UI refreshes once every 500 ms, and fetching them separately would double the inter-process round trips.
public struct HelperFeed: Codable, Sendable {
    public var logs: [LogEntry]
    /// The number of dropped log entries (the oldest are dropped when the buffer fills). The UI uses it to show "the log has gaps".
    public var droppedLogs: UInt64
    /// Key events that have occurred but not yet been consumed by the UI, given in text form.
    public var notices: [String]

    public static let empty = HelperFeed(logs: [], droppedLogs: 0, notices: [])

    public init(logs: [LogEntry], droppedLogs: UInt64, notices: [String]) {
        self.logs = logs
        self.droppedLogs = droppedLogs
        self.notices = notices
    }
}

// MARK: - Environment

/// Runtime environment preflight result.
public struct EnvironmentReport: Codable, Hashable, Sendable {
    public var isRoot: Bool
    public var sysctlsOK: Bool
    /// Specific explanation when the sysctls are unacceptable (including fix commands).
    public var sysctlDetail: String
    public var fethMaxMTU: UInt32
    public var version: String
    public var buildDescription: String
    public var libusbVersion: String

    public init(isRoot: Bool, sysctlsOK: Bool, sysctlDetail: String, fethMaxMTU: UInt32,
                version: String, buildDescription: String, libusbVersion: String) {
        self.isRoot = isRoot
        self.sysctlsOK = sysctlsOK
        self.sysctlDetail = sysctlDetail
        self.fethMaxMTU = fethMaxMTU
        self.version = version
        self.buildDescription = buildDescription
        self.libusbVersion = libusbVersion
    }
}
