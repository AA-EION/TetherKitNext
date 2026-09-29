import CTetherKitNext
import Foundation
import TetherKitNextIPC

/// The process-level interface of libtetherkitnext: version, environment preflight, device enumeration, logs.
///
/// This group **needs no root**, and both the App and the helper can call it.
public enum TetherKitNextLibrary {
    /// The maximum number of devices enumerated at once.
    ///
    /// 16 is a limit that will never be hit: plugging in 16 RNDIS devices on one machine at once is already absurd,
    /// and what the fixed-size array buys is "no need to manage memory". If it really is exceeded tk_list_devices honestly reports the total,
    /// and we just do not show the excess.
    private static let deviceCapacity = 16

    /// The maximum number of log entries taken away at once.
    ///
    /// Consistent with the C-side ring buffer's capacity (256): smaller and the backlog would never be fully drained, larger is meaningless.
    private static let logCapacity = 256

    /// Synchronizes the UI language to libtetherkitnext.
    ///
    /// It must be called -- otherwise there would be a split like "the UI is English while the library logs in the log card are Chinese".
    /// Logs already queued are not re-rendered (they were already strings at the moment they were produced),
    /// so the earlier it is called the better; on a language switch, newly produced logs are immediately in the new language.
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

    /// Runtime environment preflight. Always succeeds -- "environment unacceptable" is a result, not a failure.
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

    /// Enumerates RNDIS devices.
    ///
    /// - Parameter readStrings: Whether to read the vendor name / product name / serial number. This needs opening
    ///   the device, and during a session the device is already held exclusively, so pass false then to avoid trying in vain.
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

    // MARK: - Logs

    /// Turns on log capture and sets the level.
    public static func startLogCapture(level: LogLevel = .info) {
        tk_set_log_level(level.rawValue)
        tk_enable_log_capture(true)
    }

    public static func stopLogCapture() {
        tk_enable_log_capture(false)
    }

    /// Takes away the buffered logs. Returns the entries and "the number dropped because the buffer filled".
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

    // MARK: - Orphan NICs

    /// Cleans up feth interfaces left in the kernel after a process was forcibly killed. Needs root.
    ///
    /// The helper should call it unconditionally once at startup: if the last run was SIGKILLed, the NIC is still left in the kernel,
    /// and neither RAII nor signal handling can save that situation.
    @discardableResult
    public static func cleanupOrphanInterfaces() throws -> Int {
        var removed = 0
        var error = tk_error_t()
        let result = tk_cleanup_orphan_interfaces(&removed, &error)
        try check(result, error)
        return removed
    }
}
