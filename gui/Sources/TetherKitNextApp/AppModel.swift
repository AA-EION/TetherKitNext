import Foundation
import Observation
import SwiftUI
import TetherKitNextCore
import TetherKitNextIPC

/// The instantaneous rate obtained from one sample.
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

/// Consecutive repeated logs are folded into one.
///
/// Periodic paths (such as device enumeration once every 2 seconds) flood the same sentence into a whole column, drowning the lines that really
/// carry information; folding into "one line x N" carries the same information. The id is taken from the first entry -- repeats only update the
/// count and timestamp, and the row's identity stays unchanged, so the list need not rebuild whole rows.
struct CollapsedLogEntry: Identifiable {
    let first: LogEntry
    private(set) var latest: LogEntry
    private(set) var count: Int = 1

    init(_ entry: LogEntry) {
        first = entry
        latest = entry
    }

    var id: UUID { first.id }

    /// Only entries identical apart from the timestamp count as repeats -- messages carrying changing values (addresses, counts) should stay on separate lines.
    func matches(_ entry: LogEntry) -> Bool {
        entry.level == first.level && entry.thread == first.thread
            && entry.message == first.message
    }

    mutating func absorb(_ entry: LogEntry) {
        latest = entry
        count += 1
    }
}

/// The reachability of the helper. The UI uses it to decide between "show install guidance" and "work normally".
enum HelperAvailability: Equatable {
    case unknown
    case available(version: String)
    case missing(reason: String)
    /// The installed helper is older (or newer) than the current App, and the XPC interface does not match.
    ///
    /// A separate state rather than merged into missing: the remedies for the two cases differ,
    /// one being "go install" and the other "go reinstall", and the hint text must be able to tell them apart.
    case outdated(installed: Int, expected: Int)

    var isAvailable: Bool {
        if case .available = self { return true }
        return false
    }
}

/// The installed privileged component is not the same version as the one the App bundles.
///
/// * Division of labor with `HelperAvailability.outdated` *
///   That one is about a **protocol mismatch**: the method signatures are no longer consistent, and calling further would either hang or crash,
///   so it must be stopped at the install guidance page. This one is about **the protocol still being compatible but the version differing** --
///   the component works as usual, but it still contains the pre-upgrade libtetherkitnext, and the problems fixed in the new version have not been fixed
///   at all on the side that really does the work (`brew upgrade` only replaces the .app and cannot touch
///   /Library/PrivilegedHelperTools).
///
///   So it does not block the way, and only lights up an "Update privileged component" in the management row. For versions where the protocol number is unchanged
///   (like 0.1.4 -> 0.1.5) there used to be no hint at all, and the only sign the user could notice was
///   "the bug the release notes say was fixed is still there".
struct HelperVersionMismatch: Equatable {
    /// The installed one, taking only the version number (such as `"0.1.3"`).
    let installed: String
    /// The one the App bundles -- the version that will be installed after the button is clicked.
    let expected: String
}

/// The result of a manual "check for updates", driving a popup.
enum UpdateCheckResult: Equatable {
    case upToDate(current: String)
    case updateAvailable(UpdateChecker.Release)
    case failed(String)
    /// A development build (the bare executable from `swift run`) has no version number and cannot be compared.
    case unavailable
}

/// All of the UI's state and actions.
///
/// * Why everything goes through the helper rather than the App calling the library itself *
///   The App runs as an ordinary user, and creating feth and opening BPF both need root, so a session can only run inside
///   the helper. Device enumeration and the environment preflight, although they need no root, also go uniformly through the helper -- once a session is running
///   the device is held exclusively by the helper, and the App reading it again would only get inconsistent results.
@Observable
@MainActor
final class AppModel {
    // MARK: - State exposed to the UI

    private(set) var helperAvailability: HelperAvailability = .unknown
    /// The two version numbers when the installed component and the App version do not match; nil when consistent (or not yet probed).
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

    /// The one the user selected in the device list. nil means "use the first one found".
    var selectedDeviceID: String?
    /// The user-adjustable part of the session configuration.
    var requestedMTU: UInt32 = 1500
    var adoptDeviceMAC: Bool = true
    /// The network configuration form.
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

    /// The UI language preference. Changing it does **three** things at once: switches the Swift-side message table, pushes the language to
    /// libtetherkitnext (otherwise another language would get mixed into the log card), and then pushes it to the helper (which runs as root
    /// under launchd and cannot see the user's language preference).
    ///
    /// `didSet` also increments `languageRevision` -- messages are looked up from a global table,
    /// and SwiftUI has no way to know they changed, so this value is used to rebuild the whole view tree once.
    var languagePreference: LanguagePreference = .system {
        didSet {
            guard languagePreference != oldValue else { return }
            applyLanguage(languagePreference)
        }
    }

    /// How many times the language has changed. `.id(model.languageRevision)` is attached at the view root, and it triggers the rebuild.
    private(set) var languageRevision = 0

    /// Waiting for some privileged operation to complete -- the UI uses it to disable buttons and show progress.
    private(set) var isBusy: Bool = false
    /// An error that needs to be popped up to the user.
    var alertMessage: String?

    /// The known new version. Drives the "new version available" hint in the management row; nil = none or never checked.
    private(set) var availableUpdate: UpdateChecker.Release?
    /// The popup of the manual "check for updates" result. Set back to nil when the view dismisses the popup.
    var updateCheckResult: UpdateCheckResult?

    var logLevelFilter: LogLevel = .info

    // MARK: - Internal

    private let client = HelperClient()
    private var pollingTask: Task<Void, Never>?
    /// The previous state snapshot, used to compute rates by differencing.
    private var previousStatus: SessionStatus?
    private var sessionStartedAt: Date?
    /// The moment of the last device enumeration. A monotonic clock is used, avoiding negative intervals when the system time is adjusted.
    private var lastDeviceRefresh: ContinuousClock.Instant?

    /// Whether the main window is currently visible. It only affects the polling pace; the Dock icon policy is handled at the view layer.
    private var isWindowVisible = true

    /// The moment of the most recent "explicitly asked to show the main window". The initial value = now, because the App's launch itself
    /// is a legitimate window presentation.
    private var windowPresentationRequestedAt = ContinuousClock.now

    /// The cached authorization token. nil means the next privileged operation needs to pop up a dialog.
    ///
    /// It does not keep its own expiry time: the system's timeout is controlled by the authorization database (and may be changed by an administrator),
    /// and computing our own copy would only disagree with the system. Going by the helper's verification result is more reliable.
    private var cachedAuthorization: AuthorizationToken?

    /// The daemon refused a Touch ID-confirmed request (the user is not an
    /// administrator). Use the administrator dialog for the rest of this run.
    private var presenceRejected = false




    /// Polling period.
    ///
    /// 500 ms is the balance point between "looks real-time" and "do not turn XPC round trips into a burden": faster makes no difference
    /// to the human eye, and slower makes the rate curve look choppy.
    private static let pollInterval: Duration = .milliseconds(500)

    /// The polling interval in pure background standby (window closed, no session running).
    private static let backgroundPollInterval: Duration = .seconds(2)

    /// The minimum interval of device enumeration.
    ///
    /// Much slower than state polling, because enumeration needs libusb_open to read string descriptors. A 2-second plug/unplug
    /// response delay is basically imperceptible to the user, while the overhead drops to a quarter of the original.
    private static let deviceRefreshInterval: Duration = .seconds(2)

    /// The number of sample points the throughput curve retains. 120 points x 500 ms = the most recent 60 seconds.
    private static let historyCapacity = 120

    /// The number of lines the log panel retains.
    ///
    /// 2000 lines are enough to look back over a whole startup sequence plus several minutes of running; more than that and SwiftUI's list starts to lag.
    private static let logCapacity = 2000

    // MARK: - Lifecycle

    /// Applies a language preference: this process -> libtetherkitnext -> helper, not one place may be missed.
    ///
    /// UserDefaults is written only here, and read in `restoreLanguagePreference()` --
    /// both sides use the same key-name constant, so a rename cannot miss half.
    func applyLanguage(_ preference: LanguagePreference) {
        let resolved = L10n.apply(preference)
        TetherKitNextLibrary.setLanguage(resolved)
        UserDefaults.standard.set(preference.rawValue, forKey: Self.languageDefaultsKey)
        languageRevision += 1
        Task { await client.setLanguage(resolved) }
    }

    /// Restores the language chosen last time at startup. If never saved it is `.system`.
    private func restoreLanguagePreference() {
        let stored = UserDefaults.standard.string(forKey: Self.languageDefaultsKey)
        let preference = stored.flatMap(LanguagePreference.init(rawValue:)) ?? .system
        // Writing the stored property directly would trigger didSet to save to disk once more, so bypass it: this is a "restore",
        // not "the user changed it".
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
                // The interval is dynamic: keep it smooth while a session is running or the window is open, and slow down in pure background standby.
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

    /// The main window appears (including reopening from the menu bar).
    ///
    /// Refresh once immediately rather than waiting for the next period: background polling may be sleeping in a 2-second long interval,
    /// and the first glance the user gets when opening the window should not be stale data.
    func windowDidAppear() {
        isWindowVisible = true
        Task { await refresh() }
    }

    /// The main window closes. The App moves into background mode; polling continues (the menu bar relies on it for data) but slows down.
    func windowDidDisappear() {
        isWindowVisible = false
    }

    /// Registers that "this upcoming main window presentation is one we asked for ourselves".
    ///
    /// Registered once at App launch (initializing defaults) and once by the "open main window" button.
    func expectWindowPresentation() {
        windowPresentationRequestedAt = ContinuousClock.now
    }

    /// Whether this window appearance was requested by ourselves.
    ///
    /// SwiftUI will on its own recreate the Window scene when "an App without windows is activated" (clicking the menu bar icon triggers it),
    /// and such a revival must be closed on the spot. A time window rather than a one-shot flag is used to decide:
    /// "registered within the last 3 seconds" counts -- a one-shot flag would linger on paths like
    /// "click open again while the window is already open", while a time window heals itself naturally.
    func isWindowPresentationExpected() -> Bool {
        ContinuousClock.now - windowPresentationRequestedAt < .seconds(3)
    }

    /// The polling interval that should be used right now.
    ///
    /// Three cases use the fast pace: a session is running (the menu bar shows real-time rates), starting/stopping
    /// (the user is waiting for a result), and the window is open (the user is looking). Only "pure background standby" slows down --
    /// then the only output of polling is helper liveness probing and device scanning, and nobody needs them every half second.
    private var pollDelay: Duration {
        let sessionActive = status.runState == .running || status.runState.isTransitional
        return sessionActive || isWindowVisible ? Self.pollInterval : Self.backgroundPollInterval
    }

    // MARK: - Polling

    private func refresh() async {
        do {
            let (revision, version, build) =
                HelperConstants.decodeVersion(try await client.helperVersion())
            guard revision == HelperConstants.protocolRevision else {
                helperAvailability = .outdated(installed: revision,
                                               expected: HelperConstants.protocolRevision)
                // When the protocol does not match, do not additionally report a version mismatch: that card is meant to make the user update the component anyway,
                // and saying the same thing twice would only make people suspect there are two problems.
                helperVersionMismatch = nil
                // If the interface does not match, do not keep sending requests -- the shapes of both parameters and replies may be inconsistent.
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
            // If it cannot connect, do not send subsequent requests -- each would repeat the same failure,
            // and only flood the log.
            return
        }

        if environment == nil {
            environment = try? await client.environment()
        }

        let wasActive = (status.runState == .running || status.runState == .starting)
        if let fresh = try? await client.sessionStatus() {
            apply(status: fresh)
        }
        if let feed = try? await client.drainFeed() {
            apply(feed: feed)
        }

        // The device list is refreshed only when not running: while running the device is held exclusively and the list should not change.
        //
        // And **refreshed more slowly than state**. One enumeration has to read USB string descriptors, which needs
        // libusb_open to really open the device once -- doing this at the 500 ms state-polling pace
        // amounts to opening and closing the user's device twice a second, both wasteful and possibly disruptive to it. A 2-second slower plug/unplug response
        // does not affect the perceived experience at all.
        //
        // The exception: when an active session just ended (e.g. device unplugged), refresh immediately so the list is not stale.
        let sessionJustEnded = wasActive && (status.runState != .running && status.runState != .starting)

        if status.runState != .running, (sessionJustEnded || shouldRefreshDevices()) {
            if let fresh = try? await client.listDevices() {
                devices = fresh
                // After the device the user selected is unplugged, the selection must be invalidated with it, otherwise "Start" would look for a device by a
                // nonexistent bus address.
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

        // Record/clear the connection moment, used to display the connected duration.
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

        // The denominator uses the difference of the library's monotonic clock rather than our polling period -- the real
        // interval between two pulls gets stretched by scheduling, and using a fixed period as the denominator would overestimate the rate.
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

    // MARK: - Actions

    /// The connected duration; nil when not connected.
    var connectedDuration: TimeInterval? {
        sessionStartedAt.map { Date().timeIntervalSince($0) }
    }

    var selectedDevice: DeviceDescriptor? {
        if let selectedDeviceID {
            return devices.first { $0.id == selectedDeviceID }
        }
        return devices.first
    }

    /// The log after filtering + consecutive deduplication.
    ///
    /// Folding is placed **after** filtering: two identical INFO entries in the raw stream may have a trace between them,
    /// so folding in raw order would fail, while the adjacent repeats the user sees at some level are what should be merged.
    /// Recomputed in O(n), n <= 2000, imperceptible at a 500 ms refresh pace.
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

    /// Starts a session. Pops up the system authorization dialog once.
    func startSession() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        var configuration = SessionConfiguration(mtu: requestedMTU,
                                                 adoptDeviceMAC: adoptDeviceMAC)
        // Specify bus + address rather than VID/PID: two devices of the same model have exactly the same VID/PID,
        // and only the bus address can tell them apart.
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

    /// Stops the session.
    func stopSession() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        await authorized { [self] authorization in
            try await client.stopSession(authorization: authorization)
        }
    }

    /// Applies the network configuration.
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
            // Immediately read back once, so the UI reflects the address actually in effect right away, without waiting for the next polling period.
            networkState = (try? await client.queryNetwork(interface: interface)) ?? .empty
        }
    }

    /// Revokes the IP configuration on the NIC.
    ///
    /// A separate action rather than "choose 'do not configure' as the connectivity method and click apply": revoking is a one-time operation,
    /// and mixing it into the mode selector would make people think choosing it already takes effect.
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

    /// Manually refreshes the device list (the refresh button on the UI).
    ///
    /// Not subject to throttling when triggered manually -- the user clicking the button wants to see results immediately.
    func refreshDevices() async {
        lastDeviceRefresh = .now
        devices = (try? await client.listDevices()) ?? []
    }

    /// Whether it has been long enough since the last enumeration. Also records the moment of this one.
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

    /// The library version the App bundles -- the standard answer to "which version the privileged component should be".
    ///
    /// The **library's** version is used rather than the App version number in Info.plist: what gets installed into
    /// /Library/PrivilegedHelperTools is exactly a copy of the libtetherkitnext inside the .app
    /// (see the payload-assembly section of build-gui.sh), and comparing is more accurate when both sides share a source. Also the bare executable of `swift run`
    /// has no bundle at all, and on the path that reads Info.plist this judgment would fail entirely.
    ///
    /// It is safe to cache with `static let` because it is a constant burned into the dylib at compile time and does not
    /// change while the process lives -- unlike the several hint strings above that must follow the language. The version number extraction is also
    /// done along the way: this judgment hangs on polling that runs every 500 ms, and there is no need to rerun a regex every time.
    private static let bundledVersion =
        HelperConstants.semanticVersion(of: TetherKitNextLibrary.versionInfo.version)

    /// Whether the installed component and the App-bundled one are the same version. Returns nil when consistent.
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

    // MARK: - Checking for updates

    /// Manual check (the App menu "Check for Updates..."). The result pops up a dialog whether good or bad.
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

    /// Automatic check: at most once a day, silent on failure, and on finding a new version only lights up the hint in the management row,
    /// never popping up to interrupt -- an update is something to learn "in passing" and does not deserve a modal box.
    /// It can be turned off with `defaults write com.tetherkitnext.app updateCheckDisabled -bool YES`.
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

    /// Records the latest version found into defaults -- when tomorrow's launch is blocked by throttling, the hint should not disappear.
    private func remember(_ release: UpdateChecker.Release) {
        let defaults = UserDefaults.standard
        defaults.set(release.version, forKey: Self.updateKnownVersionKey)
        defaults.set(release.pageURL.absoluteString, forKey: Self.updateKnownPageKey)
    }

    /// Restores the new-version hint found last time at startup. After the upgrade completes (current version >= the remembered version)
    /// it naturally becomes invalid, needing no cleanup logic whatsoever.
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

    /// Executes a privileged operation carrying an authorization credential, popping up the system authorization dialog only when necessary.
    ///
    /// * Why the token is cached *
    ///   The measured parameters of `system.privilege.admin` are `shared = false` and `timeout = 300`.
    ///   `shared = false` means the credential is **not shared across AuthorizationRefs** -- creating a new ref
    ///   for every operation necessarily makes the user authenticate again, so "connect, configure network, disconnect"
    ///   would pop up the dialog three times in a row. And `timeout = 300` means the credential on the same ref
    ///   stays valid for 5 minutes.
    ///
    ///   So the token is cached and reused: the first operation pops up a dialog once, and afterwards none is needed for 5 minutes.
    ///   After expiry the helper's verification fails and explicitly says "this is an authorization problem", and on that basis we discard the
    ///   cache, pop up a dialog again, and retry this operation -- what the user sees is still "a dialog popped up
    ///   before the operation", rather than an inexplicable failure.
    ///
    /// * Why we cannot catch the credential ourselves and use it later *
    ///   The external form is only a key pointing to that authorization in securityd, not the credential itself.
    ///   Once the AuthorizationRef is released, the helper reports -60005 when restoring. So the token is held by
    ///   `cachedAuthorization`, and `withExtendedLifetime` guarantees it lives until after the
    ///   XPC round trip ends.
    ///
    /// On user cancellation **no** error hint is popped up -- cancelling is a normal action, and popping up another "cancelled" box
    /// would only be annoying.
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

        // First pass: if there is a cache, use it directly without disturbing the user.
        if let cached = cachedAuthorization {
            // withExtendedLifetime cannot take an async closure, so defer is used to pin the token until the
            // end of scope -- a local let alone is not enough, since ARC may release it right after the last read of
            // externalForm, when the XPC round trip has not yet come back.
            defer { withExtendedLifetime(cached) {} }
            do {
                try await body(cached.externalForm)
                return true
            } catch let failure as HelperClient.Failure where failure.isAuthorizationProblem {
                // The credential expired. Discard the cache and go on to "re-authorize + retry".
                cachedAuthorization = nil
            } catch {
                alertMessage = error.localizedDescription
                return false
            }
        }

        // Second pass: pop up a dialog to get a new credential, then execute (or retry).
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
