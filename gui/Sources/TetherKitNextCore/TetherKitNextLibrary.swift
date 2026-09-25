import CTetherKitNext
import Foundation
import TetherKitNextIPC

/// libtetherkitnext 的进程级接口：版本、环境预检、设备枚举、日志。
///
/// 这一组**不需要 root**，App 与 helper 都能调。
public enum TetherKitNextLibrary {
    /// 一次最多枚举多少台设备。
    ///
    /// 16 是个不会被碰到的上限：一台机器同时插 16 台 RNDIS 设备已经离谱了，
    /// 而定长数组换来的是「不用管内存」。真超了 tk_list_devices 会如实汇报总数，
    /// 我们只是不展示多出来的部分。
    private static let deviceCapacity = 16

    /// 一次最多取走多少条日志。
    ///
    /// 与 C 侧环形缓冲的容量（256）一致：小了会永远取不完积压，大了没意义。
    private static let logCapacity = 256

    /// 把界面语言同步给 libtetherkitnext。
    ///
    /// 必须调 —— 否则会出现「界面是英文、日志卡里的库日志是中文」这种割裂。
    /// 已经排队的日志不会被重新渲染（它们在产生的那一刻就已经是字符串了），
    /// 所以越早调越好；语言切换时新产生的日志立刻就是新语言。
    public static func setLanguage(_ language: Language) {
        tk_set_language(language.cValue)
    }

    public static var versionInfo: (version: String, build: String, libusb: String) {
        var info = tk_version_info_t()
        tk_version(&info)
        return (String(fixedCArray: info.text),
                String(fixedCArray: info.build),
                String(fixedCArray: info.libusb))
    }

    /// 运行环境预检。永远成功 —— 「环境不合格」是结果，不是失败。
    public static func checkEnvironment() -> EnvironmentReport {
        var raw = tk_environment_t()
        _ = tk_check_environment(&raw)
        let version = versionInfo
        return EnvironmentReport(
            isRoot: raw.is_root,
            sysctlsOK: raw.sysctls_ok,
            sysctlDetail: String(fixedCArray: raw.sysctl_detail),
            fethMaxMTU: raw.feth_max_mtu,
            version: version.version,
            buildDescription: version.build,
            libusbVersion: version.libusb)
    }

    /// 枚举 RNDIS 设备。
    ///
    /// - Parameter readStrings: 是否读取厂商名 / 产品名 / 序列号。这需要打开
    ///   设备，会话运行期间设备已被独占，那时应传 false 免得白试一遍。
    public static func listDevices(readStrings: Bool = true) throws -> [DeviceDescriptor] {
        var buffer = [tk_device_info_t](repeating: tk_device_info_t(), count: deviceCapacity)
        var count = 0
        var error = tk_error_t()

        let result = tk_list_devices(&buffer, deviceCapacity, &count, readStrings, &error)
        try check(result, error)

        return buffer.prefix(min(count, deviceCapacity)).map { raw in
            DeviceDescriptor(
                vendorID: raw.vendor_id,
                productID: raw.product_id,
                busNumber: raw.bus_number,
                deviceAddress: raw.device_address,
                manufacturer: String(fixedCArray: raw.manufacturer),
                product: String(fixedCArray: raw.product),
                serial: String(fixedCArray: raw.serial),
                summary: String(fixedCArray: raw.description),
                usedAndroidQuirk: raw.used_android_quirk)
        }
    }

    // MARK: - 日志

    /// 打开日志捕获并设定级别。
    public static func startLogCapture(level: LogLevel = .info) {
        tk_set_log_level(level.rawValue)
        tk_enable_log_capture(true)
    }

    public static func stopLogCapture() {
        tk_enable_log_capture(false)
    }

    /// 取走已缓冲的日志。返回条目与「因缓冲写满被丢弃的条数」。
    public static func drainLogs() -> (entries: [LogEntry], dropped: UInt64) {
        var buffer = [tk_log_record_t](repeating: tk_log_record_t(), count: logCapacity)
        var dropped: UInt64 = 0
        let taken = tk_drain_logs(&buffer, logCapacity, &dropped)

        let entries = buffer.prefix(taken).map { raw in
            LogEntry(
                level: LogLevel(rawValue: raw.level) ?? .info,
                timestamp: Date(timeIntervalSince1970: Double(raw.wall_nanos) / 1_000_000_000),
                thread: String(fixedCArray: raw.thread),
                message: String(fixedCArray: raw.message))
        }
        return (Array(entries), dropped)
    }

    // MARK: - 孤儿网卡

    /// 清理进程被强杀后残留在内核里的 feth 接口。需要 root。
    ///
    /// helper 应在启动时无条件调一次：上一次运行若被 SIGKILL，网卡还留在内核里，
    /// 而 RAII 与信号处理都救不了那种情况。
    @discardableResult
    public static func cleanupOrphanInterfaces() throws -> Int {
        var removed = 0
        var error = tk_error_t()
        let result = tk_cleanup_orphan_interfaces(&removed, &error)
        try check(result, error)
        return removed
    }
}
