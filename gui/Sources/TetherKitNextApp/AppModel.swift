import Foundation
import Observation
import SwiftUI
import TetherKitNextCore
import TetherKitNextIPC

/// 一次采样得到的瞬时速率。
struct ThroughputSample: Identifiable {
    let id = UUID()
    let timestamp: Date
    let receiveBitsPerSecond: Double
    let transmitBitsPerSecond: Double
    let receivePacketsPerSecond: Double
    let transmitPacketsPerSecond: Double

    static let zero = ThroughputSample(timestamp: .distantPast, receiveBitsPerSecond: 0,
                                       transmitBitsPerSecond: 0, receivePacketsPerSecond: 0,
                                       transmitPacketsPerSecond: 0)
}

/// 连续重复的日志折叠成一条。
///
/// 周期性路径（比如每 2 秒一次的设备枚举）会把同一句话刷成一整列，淹没真正
/// 有信息量的行；折叠成「一行 ×N」承载同样的信息。id 取首条的 —— 重复只更新
/// 计数与时间戳，行的身份不变，列表不用整行重建。
struct CollapsedLogEntry: Identifiable {
    let first: LogEntry
    private(set) var latest: LogEntry
    private(set) var count: Int = 1

    init(_ entry: LogEntry) {
        first = entry
        latest = entry
    }

    var id: UUID { first.id }

    /// 时间戳以外完全相同才算重复 —— 消息里带着变化的值（地址、计数）就该分行。
    func matches(_ entry: LogEntry) -> Bool {
        entry.level == first.level && entry.thread == first.thread
            && entry.message == first.message
    }

    mutating func absorb(_ entry: LogEntry) {
        latest = entry
        count += 1
    }
}

/// helper 的可达性。界面靠它决定是「显示安装引导」还是「正常工作」。
enum HelperAvailability: Equatable {
    case unknown
    case available(version: String)
    case missing(reason: String)
    /// 装着的 helper 比当前 App 旧（或新），XPC 接口对不上。
    ///
    /// 单独一个状态而不是并进 missing：这两种情况的解决办法不一样，
    /// 一个是「去装」，一个是「去重装」，提示文案必须能区分。
    case outdated(installed: Int, expected: Int)

    var isAvailable: Bool {
        if case .available = self { return true }
        return false
    }
}

/// 装着的特权组件与 App 自带的那份不是同一个版本。
///
/// ★ 与 `HelperAvailability.outdated` 的分工 ★
///   那个说的是**协议对不上**：方法签名都不一致了，再调下去要么卡住要么崩，
///   所以必须挡在安装引导页上。这个说的是**协议仍兼容、版本却不一样** ——
///   组件照常能用，只是它里面还是升级前的那份 libtetherkitnext，新版修的问题在
///   真正干活的那一侧一个都没修（`brew upgrade` 只换 .app，动不了
///   /Library/PrivilegedHelperTools）。
///
///   所以它不拦路，只在管理行里点亮一个「更新特权组件」。协议号不变的版本
///   （0.1.4 → 0.1.5 这种）以前完全没有提示，用户唯一能察觉的迹象是
///   「更新说明里写着修好的毛病还在」。
struct HelperVersionMismatch: Equatable {
    /// 装着的那份，只取版本号（如 `"0.1.3"`）。
    let installed: String
    /// App 自带的那份 —— 点了按钮之后会装上去的版本。
    let expected: String
}

/// 手动「检查更新」的结果，驱动一个弹窗。
enum UpdateCheckResult: Equatable {
    case upToDate(current: String)
    case updateAvailable(UpdateChecker.Release)
    case failed(String)
    /// 开发构建（`swift run` 的裸可执行文件）没有版本号，没法比。
    case unavailable
}

/// 界面的全部状态与动作。
///
/// ★ 为什么所有事都经过 helper，而不是 App 自己调库 ★
///   App 以普通用户身份运行，建 feth 和开 BPF 都要 root，所以会话只能在 helper
///   里跑。设备枚举与环境预检虽然不需要 root，但也统一走 helper —— 会话跑起来后
///   设备已被 helper 独占，App 再去读只会得到不一致的结果。
@Observable
@MainActor
final class AppModel {
    // MARK: - 对界面公开的状态

    private(set) var helperAvailability: HelperAvailability = .unknown
    /// 装着的组件与 App 版本对不上时的两个版本号；一致（或还没探到）为 nil。
    private(set) var helperVersionMismatch: HelperVersionMismatch?
    private(set) var environment: EnvironmentReport?
    private(set) var devices: [DeviceDescriptor] = []
    private(set) var status: SessionStatus = .idle
    private(set) var networkState: NetworkState = .empty
    private(set) var throughput: ThroughputSample = .zero
    private(set) var throughputHistory: [ThroughputSample] = []
    private(set) var logs: [LogEntry] = []
    private(set) var droppedLogCount: UInt64 = 0
    /// The daemon is registered but the user has not approved it in System
    /// Settings › Login Items yet.
    private(set) var helperNeedsApproval = false
    /// `installHelper()` is restarting the daemon; it is briefly unreachable.
    private var isRestartingHelper = false
    /// State of the `/usr/local/bin/tetherkitnext-cli` link.
    private(set) var commandLineToolState: CommandLineToolState = .current

    /// 用户在设备列表里选中的那台。为 nil 表示「用找到的第一台」。
    var selectedDeviceID: String?
    /// 会话配置里用户可调的部分。
    var requestedMTU: UInt32 = 1500
    var adoptDeviceMAC: Bool = true
    /// 网络配置表单。
    ///
    /// Persisted, so a choice like "route all traffic through this interface"
    /// sticks across launches and the automatic configuration on Connect
    /// applies it every time.
    var networkConfiguration: NetworkConfiguration = .dhcp {
        didSet {
            guard networkConfiguration != oldValue,
                  let data = try? JSONEncoder().encode(networkConfiguration) else { return }
            UserDefaults.standard.set(data, forKey: Self.networkConfigurationKey)
        }
    }

    private static let networkConfigurationKey = "networkConfiguration"

    private func restoreNetworkConfiguration() {
        guard let data = UserDefaults.standard.data(forKey: Self.networkConfigurationKey),
              let stored = try? JSONDecoder().decode(NetworkConfiguration.self, from: data)
        else { return }
        networkConfiguration = stored
    }

    /// 界面语言偏好。改它会**同时**做三件事：切 Swift 侧的文案表、把语言推给
    /// libtetherkitnext（否则日志卡里会混进另一种语言）、再推给 helper（它以 root
    /// 跑在 launchd 下，看不到用户的语言偏好）。
    ///
    /// `didSet` 里顺带把 `languageRevision` 加一 —— 文案是从全局表里查的，
    /// SwiftUI 无从得知它变了，得靠这个值把视图树整个重建一次。
    var languagePreference: LanguagePreference = .system {
        didSet {
            guard languagePreference != oldValue else { return }
            applyLanguage(languagePreference)
        }
    }

    /// 语言换了多少次。视图根上挂 `.id(model.languageRevision)`，靠它触发重建。
    private(set) var languageRevision = 0

    /// 正在等待某个特权操作完成 —— 界面据此禁用按钮并显示进度。
    private(set) var isBusy: Bool = false
    /// 需要弹给用户看的错误。
    var alertMessage: String?

    /// 已知的新版本。驱动管理行里的「有新版」提示；nil = 没有或没查过。
    private(set) var availableUpdate: UpdateChecker.Release?
    /// 手动「检查更新」的结果弹窗。视图关掉弹窗时置回 nil。
    var updateCheckResult: UpdateCheckResult?

    var logLevelFilter: LogLevel = .info

    // MARK: - 内部

    private let client = HelperClient()
    private var pollingTask: Task<Void, Never>?
    /// 上一次的状态快照，用来做差算速率。
    private var previousStatus: SessionStatus?
    private var sessionStartedAt: Date?
    /// 上次枚举设备的时刻。用单调时钟，避免系统时间被调整时算出负的间隔。
    private var lastDeviceRefresh: ContinuousClock.Instant?

    /// 主窗口当前是否可见。只影响轮询节奏；程序坞图标策略在视图层处理。
    private var isWindowVisible = true

    /// 最近一次「明确要求显示主窗口」的时刻。初始值 = 现在，因为 App 启动本身
    /// 就是一次合法的窗口展示。
    private var windowPresentationRequestedAt = ContinuousClock.now

    /// 缓存的授权令牌。为 nil 表示下次特权操作需要弹框。
    ///
    /// 不自己记过期时间：系统的 timeout 由授权数据库控制（可能被管理员改），
    /// 我们自己算一份只会和系统不一致。以 helper 的复核结果为准更可靠。
    private var cachedAuthorization: AuthorizationToken?

    /// The daemon refused a Touch ID-confirmed request (the user is not an
    /// administrator). Use the administrator dialog for the rest of this run.
    private var presenceRejected = false




    /// 轮询周期。
    ///
    /// 500 ms 是「看起来实时」与「别把 XPC 往返变成负担」的平衡点：更快对人眼
    /// 已无区别，更慢会让速率曲线看起来一顿一顿的。
    private static let pollInterval: Duration = .milliseconds(500)

    /// 纯后台待机（窗口关着、会话没跑）时的轮询间隔。
    private static let backgroundPollInterval: Duration = .seconds(2)

    /// 设备枚举的最小间隔。
    ///
    /// 比状态轮询慢得多，因为枚举要 libusb_open 去读字符串描述符。2 秒的插拔
    /// 响应延迟用户基本感觉不到，而开销降到了原来的四分之一。
    private static let deviceRefreshInterval: Duration = .seconds(2)

    /// 吞吐曲线保留的采样点数。120 点 × 500 ms = 最近 60 秒。
    private static let historyCapacity = 120

    /// 日志面板保留的行数。
    ///
    /// 2000 行足够回溯一整次启动序列加若干分钟运行；再多 SwiftUI 的列表会开始卡。
    private static let logCapacity = 2000

    // MARK: - 生命周期

    /// 应用一个语言偏好：本进程 → libtetherkitnext → helper，一处都不能漏。
    ///
    /// UserDefaults 只在这里写，读在 `restoreLanguagePreference()` 里 ——
    /// 两边都走同一个键名常量，改名不会漏改一半。
    func applyLanguage(_ preference: LanguagePreference) {
        let resolved = L10n.apply(preference)
        TetherKitNextLibrary.setLanguage(resolved)
        UserDefaults.standard.set(preference.rawValue, forKey: Self.languageDefaultsKey)
        languageRevision += 1
        Task { await client.setLanguage(resolved) }
    }

    /// 启动时恢复上次选的语言。没存过就是 `.system`。
    private func restoreLanguagePreference() {
        let stored = UserDefaults.standard.string(forKey: Self.languageDefaultsKey)
        let preference = stored.flatMap(LanguagePreference.init(rawValue:)) ?? .system
        // 直接写存储属性会触发 didSet 再存一次盘，绕开它：这里是「恢复」，
        // 不是「用户改了」。
        if preference != languagePreference {
            languagePreference = preference
        } else {
            applyLanguage(preference)
        }
    }

    private static let languageDefaultsKey = "TetherKitNextLanguagePreference"

    func start() {
        guard pollingTask == nil else { return }
        restoreLanguagePreference()
        restoreNetworkConfiguration()
        pollingTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                await self.refresh()
                // 间隔是动态的：会话在跑或窗口开着就保持流畅，纯后台待机时放慢。
                try? await Task.sleep(for: self.pollDelay)
            }
        }
        restoreKnownUpdate()
        Task { await checkForUpdatesQuietly() }
    }

    func stopPolling() {
        pollingTask?.cancel()
        pollingTask = nil
    }

    /// 主窗口出现（含从菜单栏重新打开）。
    ///
    /// 立刻刷一次而不是等下一个周期：后台轮询可能正睡在 2 秒的长间隔里，
    /// 用户打开窗口的第一眼不该看到陈旧数据。
    func windowDidAppear() {
        isWindowVisible = true
        Task { await refresh() }
    }

    /// 主窗口关闭。App 转入后台模式，轮询继续（菜单栏靠它喂数据）但会放慢。
    func windowDidDisappear() {
        isWindowVisible = false
    }

    /// 登记「接下来这次主窗口展示是我们主动要求的」。
    ///
    /// App 启动时（初始化默认值）与「打开主窗口」按钮各登记一次。
    func expectWindowPresentation() {
        windowPresentationRequestedAt = ContinuousClock.now
    }

    /// 这次窗口出现是不是我们自己要求的。
    ///
    /// SwiftUI 会在「无窗口的 App 被激活」时（点菜单栏图标就会触发）擅自重建
    /// Window 场景，那种复活必须当场关掉。用时间窗而不是一次性标志来判定：
    /// 「3 秒内登记过」就算数 —— 一次性标志在「窗口已开着时再点打开」这类
    /// 路径上会残留，时间窗天然自愈。
    func isWindowPresentationExpected() -> Bool {
        ContinuousClock.now - windowPresentationRequestedAt < .seconds(3)
    }

    /// 当前该用的轮询间隔。
    ///
    /// 三种情况用快节奏：会话在跑（菜单栏要显示实时速率）、正在启动/停止
    /// （用户在等结果）、窗口开着（用户在看）。只有「纯后台待机」才放慢 ——
    /// 那时轮询唯一的产出是 helper 探活与设备扫描，没人需要它们每半秒一次。
    private var pollDelay: Duration {
        let sessionActive = status.runState == .running || status.runState.isTransitional
        return sessionActive || isWindowVisible ? Self.pollInterval : Self.backgroundPollInterval
    }

    // MARK: - 轮询

    private func refresh() async {
        do {
            let (revision, version, build) =
                HelperConstants.decodeVersion(try await client.helperVersion())
            guard revision == HelperConstants.protocolRevision else {
                helperAvailability = .outdated(installed: revision,
                                               expected: HelperConstants.protocolRevision)
                // 协议对不上时不再另报版本不一致：那张卡本来就是要用户去更新组件，
                // 同一件事说两遍只会让人怀疑是两个问题。
                helperVersionMismatch = nil
                // 接口对不上就别继续发请求了 —— 参数与应答的形状都可能不一致。
                return
            }
            helperAvailability = .available(version: version)
            helperVersionMismatch = Self.versionMismatch(installed: version, build: build)
            helperNeedsApproval = false
            commandLineToolState = .current
        } catch {
            // While the daemon restarts it is briefly unreachable. Keep what we
            // had instead of flashing the setup steps as if it were not installed.
            if isRestartingHelper { return }
            helperAvailability = .missing(reason: error.localizedDescription)
            helperVersionMismatch = nil
            helperNeedsApproval = HelperInstaller.needsApproval
            // 连不上就别再发后续请求了 —— 每一个都会重复同样的失败，
            // 只会把日志刷满。
            return
        }

        if environment == nil {
            environment = try? await client.environment()
        }

        if let fresh = try? await client.sessionStatus() {
            apply(status: fresh)
        }
        if let feed = try? await client.drainFeed() {
            apply(feed: feed)
        }

        // 设备列表只在没跑起来的时候刷：运行中设备已被独占，列表也不该变。
        //
        // 而且**刷得比状态慢**。枚举一次要读 USB 字符串描述符，那需要
        // libusb_open 真的把设备打开一遍 —— 按 500 ms 的状态轮询节奏做这件事，
        // 等于每秒钟去开关用户的设备两次，既浪费又可能干扰它。插拔响应慢 2 秒
        // 完全不影响体感。
        if status.runState != .running, shouldRefreshDevices() {
            if let fresh = try? await client.listDevices() {
                devices = fresh
                // 用户选中的设备被拔掉后，选择要跟着失效，否则「启动」会按一个
                // 不存在的总线地址去找设备。
                if let selected = selectedDeviceID,
                   !devices.contains(where: { $0.id == selected }) {
                    selectedDeviceID = nil
                }
            }
        }

        if !status.systemInterface.isEmpty {
            networkState = (try? await client.queryNetwork(interface: status.systemInterface))
                ?? .empty
        } else {
            networkState = .empty
        }
    }


    private func apply(status fresh: SessionStatus) {
        defer {
            previousStatus = fresh
            status = fresh
        }

        // 记录/清除连接时刻，用于显示已连接时长。
        if fresh.runState == .running, sessionStartedAt == nil {
            sessionStartedAt = Date()
        } else if fresh.runState != .running, fresh.runState != .stopping {
            sessionStartedAt = nil
        }

        guard let previous = previousStatus,
              fresh.runState == .running,
              fresh.monotonicNanos > previous.monotonicNanos else {
            if fresh.runState != .running {
                throughput = .zero
            }
            return
        }

        // 分母用库那边的单调时钟差，而不是我们的轮询周期 —— 两次拉取之间的
        // 真实间隔会被调度拉长，用固定周期当分母会把速率算高。
        let seconds = Double(fresh.monotonicNanos - previous.monotonicNanos) / 1_000_000_000
        guard seconds > 0 else { return }

        let sample = ThroughputSample(
            timestamp: Date(),
            receiveBitsPerSecond: Double(fresh.rxBytes &- previous.rxBytes) * 8 / seconds,
            transmitBitsPerSecond: Double(fresh.txBytes &- previous.txBytes) * 8 / seconds,
            receivePacketsPerSecond: Double(fresh.rxFrames &- previous.rxFrames) / seconds,
            transmitPacketsPerSecond: Double(fresh.txFrames &- previous.txFrames) / seconds)

        throughput = sample
        throughputHistory.append(sample)
        if throughputHistory.count > Self.historyCapacity {
            throughputHistory.removeFirst(throughputHistory.count - Self.historyCapacity)
        }
    }

    private func apply(feed: HelperFeed) {
        if !feed.logs.isEmpty {
            logs.append(contentsOf: feed.logs)
            if logs.count > Self.logCapacity {
                logs.removeFirst(logs.count - Self.logCapacity)
            }
        }
        droppedLogCount += feed.droppedLogs
    }

    // MARK: - 动作

    /// 已连接时长；未连接时为 nil。
    var connectedDuration: TimeInterval? {
        sessionStartedAt.map { Date().timeIntervalSince($0) }
    }

    var selectedDevice: DeviceDescriptor? {
        if let selectedDeviceID {
            return devices.first { $0.id == selectedDeviceID }
        }
        return devices.first
    }

    /// 过滤 + 连续去重后的日志。
    ///
    /// 折叠放在过滤**之后**：原始流里两条相同的 INFO 之间可能夹着 trace，
    /// 按原始顺序折叠会失效，而用户在某个级别下看到的相邻重复才是该合并的。
    /// O(n) 重算，n ≤ 2000，对 500 ms 的刷新节奏无感。
    var filteredLogs: [CollapsedLogEntry] {
        var collapsed: [CollapsedLogEntry] = []
        for entry in logs where entry.level >= logLevelFilter {
            if let last = collapsed.last, last.matches(entry) {
                collapsed[collapsed.count - 1].absorb(entry)
            } else {
                collapsed.append(CollapsedLogEntry(entry))
            }
        }
        return collapsed
    }

    /// 启动会话。会弹一次系统授权框。
    func startSession() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        var configuration = SessionConfiguration(mtu: requestedMTU,
                                                 adoptDeviceMAC: adoptDeviceMAC)
        // 指定总线 + 地址而不是 VID/PID：同型号两台设备 VID/PID 完全一样，
        // 只有总线地址能区分。
        if let device = selectedDevice {
            configuration.busNumber = device.busNumber
            configuration.deviceAddress = device.deviceAddress
        }

        let started = await authorized { [self] authorization in
            throughputHistory.removeAll()
            try await client.startSession(authorization: authorization,
                                          configuration: configuration)
        }
        if started, UserDefaults.standard.object(forKey: Self.autoConfigureNetworkKey) as? Bool
            ?? true {
            await configureNetworkAfterConnect()
        }
    }

    /// Defaults key for "configure the network automatically on connect".
    static let autoConfigureNetworkKey = "autoConfigureNetworkOnConnect"

    /// One-click internet: once the virtual interface exists, apply the
    /// addressing mode chosen on the Network page — DHCP unless the user set
    /// up something else — so Connect alone brings the tethered link up.
    ///
    /// Runs inside startSession's busy window and reuses the authorization
    /// token Connect just obtained, so there is no second password prompt.
    /// Skipped when the mode is "don't configure", when a static form is
    /// incomplete, or when the interface already has an address (e.g. a
    /// persistent network service picked it up by itself).
    private func configureNetworkAfterConnect() async {
        guard networkConfiguration.mode != .none,
              NetworkValidator.validationMessage(for: networkConfiguration) == nil else { return }

        // The session reaches running (and names its interface) only after the
        // RNDIS handshake and feth creation; poll for that, bounded.
        var interface = ""
        for _ in 0..<60 {
            guard let fresh = try? await client.sessionStatus() else { return }
            if fresh.runState == .failed || fresh.runState == .stopped { return }
            if fresh.runState == .running, !fresh.systemInterface.isEmpty {
                interface = fresh.systemInterface
                break
            }
            try? await Task.sleep(for: .milliseconds(250))
        }
        guard !interface.isEmpty else { return }

        if let current = try? await client.queryNetwork(interface: interface),
           current.hasAddress {
            networkState = current
            return
        }

        let configuration = networkConfiguration
        await authorized { [self] authorization in
            try await client.applyNetwork(authorization: authorization, interface: interface,
                                          configuration: configuration)
            networkState = (try? await client.queryNetwork(interface: interface)) ?? .empty
        }
    }

    /// 停止会话。
    func stopSession() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        await authorized { [self] authorization in
            try await client.stopSession(authorization: authorization)
        }
    }

    /// 下发网络配置。
    func applyNetworkConfiguration() async {
        guard !isBusy else { return }
        let interface = status.systemInterface
        guard !interface.isEmpty else {
            alertMessage = L(.interfaceNotReadyYet)
            return
        }
        if let message = NetworkValidator.validationMessage(for: networkConfiguration) {
            alertMessage = message
            return
        }

        isBusy = true
        defer { isBusy = false }

        await authorized { [self] authorization in
            try await client.applyNetwork(authorization: authorization, interface: interface,
                                          configuration: networkConfiguration)
            // 立刻回读一次，让界面马上反映真实生效的地址，而不用等下一个轮询周期。
            networkState = (try? await client.queryNetwork(interface: interface)) ?? .empty
        }
    }

    /// 撤销网卡上的 IP 配置。
    ///
    /// 单独一个动作而不是「上网方式选『不配置』再点应用」：撤销是一次性操作，
    /// 混进模式选择器里会让人以为选中它就已经生效了。
    func clearNetworkConfiguration() async {
        guard !isBusy else { return }
        let interface = status.systemInterface
        guard !interface.isEmpty else { return }

        isBusy = true
        defer { isBusy = false }

        await authorized { [self] authorization in
            try await client.applyNetwork(authorization: authorization, interface: interface,
                                          configuration: NetworkConfiguration(mode: .none))
            networkState = (try? await client.queryNetwork(interface: interface)) ?? .empty
        }
    }

    /// 手动刷新设备列表（界面上的刷新按钮）。
    ///
    /// 手动触发时不受节流限制 —— 用户点了按钮就是想立刻看到结果。
    func refreshDevices() async {
        lastDeviceRefresh = .now
        devices = (try? await client.listDevices()) ?? []
    }

    /// 距上次枚举是否已经够久。顺带记下这一次的时刻。
    private func shouldRefreshDevices() -> Bool {
        let now = ContinuousClock.now
        if let last = lastDeviceRefresh, now - last < Self.deviceRefreshInterval {
            return false
        }
        lastDeviceRefresh = now
        return true
    }

    func clearLogs() {
        logs.removeAll()
        droppedLogCount = 0
    }

    /// App 自带的库版本 —— 「特权组件应该是哪一版」的标准答案。
    ///
    /// 用**库**的版本而不是 Info.plist 里的 App 版本号：装到
    /// /Library/PrivilegedHelperTools 的正是 .app 里那份 libtetherkitnext 的拷贝
    /// （见 build-gui.sh 组装载荷那段），两边同源才比得准。而且 `swift run`
    /// 的裸可执行文件根本没有 bundle，读 Info.plist 那条路上这个判断会整个失效。
    ///
    /// 敢用 `static let` 缓存是因为它是编译期烧进 dylib 的常量，进程活着的时候
    /// 不会变 —— 与上面几个必须跟着语言走的提示语不同。顺手把版本号提取也
    /// 做掉：这个判断挂在 500 ms 一次的轮询上，没必要每次都重跑一遍正则。
    private static let bundledVersion =
        HelperConstants.semanticVersion(of: TetherKitNextLibrary.versionInfo.version)

    /// 装着的组件和 App 自带的是不是同一版。一致时返回 nil。
    ///
    /// Also compares build IDs: two builds can share a version number (every
    /// 0.2.0 prerelease), and the daemon keeps running the old binary after the
    /// app is replaced until it is restarted. Without this, a same-version
    /// update never offered the restart and the new daemon code never ran.
    private static func versionMismatch(installed: String, build: String) -> HelperVersionMismatch? {
        let installedVersion = HelperConstants.semanticVersion(of: installed)
        let installedBuild = HelperConstants.buildID(of: build)
        let differentBuild = installedBuild != nil && bundledBuild != nil
            && installedBuild != bundledBuild
        guard installedVersion != bundledVersion || differentBuild else { return nil }
        if installedVersion == bundledVersion, let installedBuild, let bundledBuild {
            return HelperVersionMismatch(installed: "\(installedVersion) (\(installedBuild))",
                                         expected: "\(bundledVersion) (\(bundledBuild))")
        }
        return HelperVersionMismatch(installed: installedVersion, expected: bundledVersion)
    }

    private static let bundledBuild =
        HelperConstants.buildID(of: TetherKitNextLibrary.versionInfo.build)

    /// Registers the daemon with SMAppService (onboarding card), or restarts
    /// it from this app bundle when its version differs ("Update helper").
    ///
    /// No password prompt: macOS asks the user to approve the background item
    /// in System Settings › Login Items instead. When approval is pending we
    /// open that pane; polling picks the daemon up as soon as it is enabled.
    func installHelper() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        let restarting = helperAvailability.isAvailable
            || HelperInstaller.status == .enabled
        isRestartingHelper = restarting
        defer { isRestartingHelper = false }
        do {
            if restarting {
                try await HelperInstaller.reregister()
            } else {
                try HelperInstaller.register()
            }
        } catch {
            isRestartingHelper = false
            await refresh()
            alertMessage = error.localizedDescription
            return
        }

        helperNeedsApproval = HelperInstaller.needsApproval
        if helperNeedsApproval {
            isRestartingHelper = false
            await refresh()
            HelperInstaller.openApprovalSettings()
            return
        }
        // launchd starts the daemon on the first connection; give it time.
        for _ in 0..<20 {
            await refresh()
            if helperAvailability.isAvailable, helperVersionMismatch == nil { return }
            try? await Task.sleep(for: .milliseconds(500))
        }
        isRestartingHelper = false
        await refresh()
    }

    /// Unregisters the daemon. launchd sends it SIGTERM, which stops a running
    /// session and destroys the virtual interfaces (the UI warns first).
    func uninstallHelper() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        // Remove our CLI link while the daemon can still do it; best effort.
        if commandLineToolState == .installed {
            await authorized(prompt: L(.authPromptCommandLineTool),
                         reason: L(.presenceReasonCommandLineTool)) { [self] authorization in
                try await client.setCommandLineToolInstalled(authorization: authorization,
                                                             install: false)
            }
        }
        do {
            try await HelperInstaller.unregister()
        } catch {
            alertMessage = error.localizedDescription
        }
        cachedAuthorization = nil
        UserPresence.reset()
        helperNeedsApproval = false
        commandLineToolState = .current
        await refresh()
    }

    /// Opens System Settings › Login Items (approval card button).
    func openHelperApprovalSettings() {
        HelperInstaller.openApprovalSettings()
    }

    // MARK: - Command-line tool

    /// Links or unlinks `/usr/local/bin/tetherkitnext-cli` → the CLI in this bundle.
    func setCommandLineToolInstalled(_ install: Bool) async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        await authorized(prompt: L(.authPromptCommandLineTool),
                         reason: L(.presenceReasonCommandLineTool)) { [self] authorization in
            try await client.setCommandLineToolInstalled(authorization: authorization,
                                                         install: install)
        }
        commandLineToolState = .current
    }

    // MARK: - 检查更新

    /// 手动检查（App 菜单「检查更新…」）。结果无论好坏都弹窗。
    func checkForUpdates() async {
        guard let current = UpdateChecker.currentVersion else {
            updateCheckResult = .unavailable
            return
        }
        do {
            let latest = try await UpdateChecker.fetchLatestRelease()
            remember(latest)
            if UpdateChecker.isNewer(latest.version, than: current) {
                availableUpdate = latest
                updateCheckResult = .updateAvailable(latest)
            } else {
                availableUpdate = nil
                updateCheckResult = .upToDate(current: current)
            }
        } catch {
            updateCheckResult = .failed(error.localizedDescription)
        }
    }

    /// 自动检查：每天至多一次、失败静默、发现新版只点亮管理行的提示，
    /// 绝不弹窗打断 —— 更新是「顺便知道」的事，不值得一个模态框。
    /// `defaults write com.tetherkitnext.app updateCheckDisabled -bool YES` 可关掉。
    private func checkForUpdatesQuietly() async {
        guard let current = UpdateChecker.currentVersion else { return }
        let defaults = UserDefaults.standard
        guard !defaults.bool(forKey: Self.updateCheckDisabledKey) else { return }
        if let last = defaults.object(forKey: Self.updateLastCheckedKey) as? Date,
           Date().timeIntervalSince(last) < 24 * 60 * 60 {
            return
        }

        guard let latest = try? await UpdateChecker.fetchLatestRelease() else { return }
        defaults.set(Date(), forKey: Self.updateLastCheckedKey)
        remember(latest)
        availableUpdate = UpdateChecker.isNewer(latest.version, than: current) ? latest : nil
    }

    /// 把查到的最新版记进 defaults —— 明天的启动被节流拦住时，提示不该消失。
    private func remember(_ release: UpdateChecker.Release) {
        let defaults = UserDefaults.standard
        defaults.set(release.version, forKey: Self.updateKnownVersionKey)
        defaults.set(release.pageURL.absoluteString, forKey: Self.updateKnownPageKey)
    }

    /// 启动时恢复上次查到的新版提示。升级完成后（当前版本 ≥ 记住的版本）
    /// 自然失效，不需要任何清理逻辑。
    private func restoreKnownUpdate() {
        guard let current = UpdateChecker.currentVersion,
              let version = UserDefaults.standard.string(forKey: Self.updateKnownVersionKey),
              let page = UserDefaults.standard.string(forKey: Self.updateKnownPageKey),
              let pageURL = URL(string: page), pageURL.scheme == "https",
              pageURL.host == "github.com",
              UpdateChecker.isNewer(version, than: current) else { return }
        availableUpdate = UpdateChecker.Release(version: version, pageURL: pageURL)
    }

    private static let updateCheckDisabledKey = "updateCheckDisabled"
    private static let updateLastCheckedKey = "updateLastCheckedAt"
    private static let updateKnownVersionKey = "updateKnownVersion"
    private static let updateKnownPageKey = "updateKnownPageURL"

    /// 带着授权凭据执行一次特权操作，必要时才弹系统授权框。
    ///
    /// ★ 为什么缓存令牌 ★
    ///   `system.privilege.admin` 的实测参数是 `shared = false`、`timeout = 300`。
    ///   `shared = false` 意味着凭据**不跨 AuthorizationRef 共享** —— 每次操作
    ///   新建一个 ref，就必然要用户重新认证一次，于是「连接、配网络、断开」
    ///   会连弹三次框。而 `timeout = 300` 意味着同一个 ref 上的凭据 5 分钟内
    ///   一直有效。
    ///
    ///   所以缓存令牌复用：第一次操作弹一次框，之后 5 分钟内都不用再弹。
    ///   过期后 helper 的复核会失败并明确告知「这是授权问题」，我们据此丢弃
    ///   缓存、重新弹一次框、把这次操作重试一遍 —— 用户看到的仍然是「操作前
    ///   弹了一次框」，而不是一个莫名其妙的失败。
    ///
    /// ★ 为什么不能自己接住凭据再用 ★
    ///   外部形式只是指向 securityd 里那份授权的一把钥匙，不是凭据本身。
    ///   AuthorizationRef 一释放，helper 还原时就会报 -60005。所以令牌由
    ///   `cachedAuthorization` 持有，并用 `withExtendedLifetime` 保证它活到
    ///   XPC 往返结束之后。
    ///
    /// 用户取消时**不**弹错误提示 —— 取消是正常操作，再弹一个「已取消」的框
    /// 只会烦人。
    /// Returns whether `body` ran to completion.
    @discardableResult
    private func authorized(prompt: String = L(.authPromptSession),
                            reason: String = L(.presenceReasonSession),
                            _ body: @escaping (Data) async throws -> Void) async -> Bool {
        // Team-signed builds: Touch ID (or the login password) instead of the
        // administrator dialog, and no authorization data at all. See
        // UserPresence for why that is safe. The daemon still refuses it for
        // non-admin users; then we fall through to the admin dialog below, and
        // stop trying for the rest of this run.
        if UserPresence.isAvailable && !presenceRejected {
            do {
                try await UserPresence.confirm(reason: reason)
                try await body(Data())
                return true
            } catch UserPresence.Failure.cancelled {
                return false
            } catch UserPresence.Failure.unavailable {
                // No Touch ID and no password fallback: use the admin dialog.
            } catch let failure as HelperClient.Failure where failure.isAuthorizationProblem {
                presenceRejected = true
            } catch {
                alertMessage = error.localizedDescription
                return false
            }
        }

        // 第一趟：有缓存就直接用，不打扰用户。
        if let cached = cachedAuthorization {
            // withExtendedLifetime 不能接 async 闭包，所以用 defer 把令牌钉到
            // 作用域结束 —— 光靠局部 let 不够，ARC 可以在最后一次读
            // externalForm 之后就释放它，而那时 XPC 往返还没回来。
            defer { withExtendedLifetime(cached) {} }
            do {
                try await body(cached.externalForm)
                return true
            } catch let failure as HelperClient.Failure where failure.isAuthorizationProblem {
                // 凭据过期了。丢掉缓存，往下走「重新授权 + 重试」。
                cachedAuthorization = nil
            } catch {
                alertMessage = error.localizedDescription
                return false
            }
        }

        // 第二趟：弹框取新凭据，然后执行（或重试）。
        do {
            let token = try AuthorizationBroker.requestAuthorization(prompt: prompt)
            defer { withExtendedLifetime(token) {} }
            cachedAuthorization = token
            try await body(token.externalForm)
            return true
        } catch AuthorizationBroker.Failure.userCancelled {
            return false
        } catch {
            alertMessage = error.localizedDescription
            return false
        }
    }
}
