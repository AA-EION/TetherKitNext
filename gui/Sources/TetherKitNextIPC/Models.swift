import Foundation

// App 与 helper 之间传输的数据模型。
//
// ★ 为什么全部走 Codable + Data，而不是 NSSecureCoding 类 ★
//   XPC 只接受 plist 类型或实现了 NSSecureCoding 的类，后者要求两端注册允许的
//   类集合，写错一个就是运行时的「不允许的类」异常，而且报错信息很难定位。
//   直接把 Codable 结构体编成 JSON Data 传过去，两端共用同一份类型定义，
//   校验交给 JSONDecoder —— 简单，且加字段天然向后兼容。

// MARK: - 会话

/// RNDIS 会话的启动配置。字段与 C ABI 的 tk_session_config_t 一一对应。
public struct SessionConfiguration: Codable, Hashable, Sendable {
    /// 设备筛选。全为 0 表示用第一个找到的 RNDIS 设备。
    public var vendorID: UInt16
    public var productID: UInt16
    /// 总线号与设备地址必须同时给出，用于区分两台同型号设备。
    public var busNumber: UInt8
    public var deviceAddress: UInt8

    /// 希望使用的 MTU。设备装不下时会被协商下调。
    public var mtu: UInt32
    /// 是否把系统侧网卡的 MAC 设为设备汇报的地址。
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

/// 会话生命周期状态。原始值与 C 的 tk_run_state_t 对齐。
public enum RunState: Int32, Codable, Sendable {
    case idle = 0
    case starting = 1
    case running = 2
    case stopping = 3
    case stopped = 4
    case failed = 5

    /// 是否处于「正在忙」的过渡态 —— 界面据此禁用按钮并显示进度。
    public var isTransitional: Bool { self == .starting || self == .stopping }
}

/// RNDIS 主机侧状态。原始值与 C 的 tk_rndis_state_t 对齐。
public enum RndisState: Int32, Codable, Sendable {
    case uninitialized = 0
    case initializing = 1
    case initialized = 2
    /// 数据开始流动。
    case dataInitialized = 3
    case halting = 4
}

/// 会话状态快照。
public struct SessionStatus: Codable, Hashable, Sendable {
    public var runState: RunState
    public var rndisState: RndisState
    public var linkUp: Bool
    /// 数据搬运是否处于暂停（链路 down 或设备软复位期间）。
    public var paused: Bool

    /// 系统侧网卡名 —— 用户要在这张上配 IP。
    public var systemInterface: String
    /// 驱动侧网卡名，仅用于排障展示。
    public var driverInterface: String

    /// 形如 "aa:bb:cc:dd:ee:ff"；未协商时为空。
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

    /// 采样时刻的单调纳秒计数。**算速率必须用它做分母**，不能用界面定时器的
    /// 周期 —— 两次拉取之间的真实间隔会被调度拉长，那样算出来的速率偏高。
    public var monotonicNanos: Int64

    /// runState == .failed 时的原因。
    public var fatalMessage: String

    /// 未连接时的空状态。
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

// MARK: - 设备

/// 一台被识别为 RNDIS 的 USB 设备。
public struct DeviceDescriptor: Codable, Hashable, Identifiable, Sendable {
    public var vendorID: UInt16
    public var productID: UInt16
    public var busNumber: UInt8
    public var deviceAddress: UInt8
    /// 厂商名 / 产品名 / 序列号是尽力而为的：读它们要打开设备，设备被占用时
    /// 会拿不到 —— 但 helper 会回填上次成功读到的值，所以连接前后名字保持
    /// 稳定。连回填值都没有（helper 启动后从未读到过）才是空串，界面此时
    /// 回落到显示 `description`。
    public var manufacturer: String
    public var product: String
    public var serial: String
    /// 形如 "Bus 020 Device 003: 18d1:4ee4"，任何情况下都可用。
    public var summary: String
    public var usedAndroidQuirk: Bool

    /// 总线 + 地址唯一确定一台已连接的设备；同型号两台也能区分。
    public var id: String { "\(busNumber).\(deviceAddress)" }

    /// 界面上显示的主标题。
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

// MARK: - 网络配置

/// 上网方式。原始值与 C 的 tk_ip_mode_t 对齐。
public enum IPMode: Int32, Codable, CaseIterable, Sendable {
    case dhcp = 0
    case manual = 1
    /// 撤销配置。
    case none = 2

    public var displayName: String {
        switch self {
        case .dhcp: return L(.ipModeDhcp)
        case .manual: return L(.ipModeManual)
        case .none: return L(.ipModeNone)
        }
    }
}

/// 网卡的上网方式配置。
public struct NetworkConfiguration: Codable, Hashable, Sendable {
    public var mode: IPMode
    /// 以下四项仅在 mode == .manual 时使用。
    public var address: String
    public var netmask: String
    public var router: String
    public var dnsServers: [String]
    /// 是否把**全局**默认路由也指向本网卡。
    ///
    /// 不开时只有一条绑定到本接口的 scoped 默认路由。只有在同时存在更高优先级的
    /// 连通服务（Wi-Fi、VPN）时才需要开 —— 而 USB 网络共享的典型场景恰恰是
    /// 没有别的网络可用，那时本网卡自然就是主服务。
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

/// 网卡当前**真实生效**的状态。
///
/// 刻意不复述「我们下发了什么」而是回读系统：静态模式下 DNS 能不能生效取决于
/// IPMonitor 认不认这个服务，只有回读才能给用户准确的反馈。
public struct NetworkState: Codable, Hashable, Sendable {
    public var hasAddress: Bool
    public var address: String
    public var netmask: String
    public var router: String
    public var dnsServers: [String]
    /// IPConfiguration 汇报的配置方式（"DHCP" / "MANUAL" / ""）。
    public var method: String
    /// IPConfiguration 汇报的服务状态（如 "BOUND"）。
    public var serviceState: String
    public var hasDefaultRoute: Bool
    /// 全局默认路由当前是否指向本网卡。
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

// MARK: - 日志与事件

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

/// helper 一次「取走待处理内容」的应答。
///
/// 事件与日志合并成一次调用，是为了把 XPC 往返次数压到每个刷新周期一次 ——
/// 界面每 500 ms 刷一次，分开取就是双倍的进程间往返。
public struct HelperFeed: Codable, Sendable {
    public var logs: [LogEntry]
    /// 被丢弃的日志条数（缓冲写满时丢最旧的）。界面据此提示「日志有缺口」。
    public var droppedLogs: UInt64
    /// 已发生但尚未被界面消费的关键事件，用文字形式给出。
    public var notices: [String]

    public static let empty = HelperFeed(logs: [], droppedLogs: 0, notices: [])

    public init(logs: [LogEntry], droppedLogs: UInt64, notices: [String]) {
        self.logs = logs
        self.droppedLogs = droppedLogs
        self.notices = notices
    }
}

// MARK: - 环境

/// 运行环境预检结果。
public struct EnvironmentReport: Codable, Hashable, Sendable {
    public var isRoot: Bool
    public var sysctlsOK: Bool
    /// sysctl 不合格时的具体说明（含修正命令）。
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
