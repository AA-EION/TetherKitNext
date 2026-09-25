import AppKit
import SwiftUI
import TetherKitIPC

/// Sidebar destinations.
///
/// The old single dashboard crammed five cards into one screen under a hard
/// "fits in 700 pt" budget. Splitting by task — look at the connection, pick a
/// device, set up addressing, read logs, manage the app — gives each page room
/// and makes the window resizable without layout gymnastics.
enum Pane: String, CaseIterable, Identifiable, Hashable {
    case overview, device, network, activity, settings

    var id: Self { self }

    var title: String {
        switch self {
        case .overview: return L(.paneOverview)
        case .device: return L(.paneDevice)
        case .network: return L(.paneNetwork)
        case .activity: return L(.paneActivity)
        case .settings: return L(.paneSettings)
        }
    }

    var symbol: String {
        switch self {
        case .overview: return "gauge.with.dots.needle.67percent"
        case .device: return "cable.connector"
        case .network: return "network"
        case .activity: return "list.bullet.rectangle"
        case .settings: return "gearshape"
        }
    }
}

struct ContentView: View {
    @Bindable var model: AppModel

    @SceneStorage("selectedPane") private var pane: Pane = .overview
    @State private var confirmingHelperUninstall = false
    @State private var confirmingHelperUpdate = false

    var body: some View {
        Group {
            if model.helperAvailability.isAvailable {
                mainSplit
            } else {
                OnboardingView(model: model)
            }
        }
        .animation(.smooth(duration: 0.25), value: model.helperAvailability)
        .alert(L(.alertOperationFailed),
               isPresented: Binding(get: { model.alertMessage != nil },
                                    set: { if !$0 { model.alertMessage = nil } })) {
            Button(L(.ok)) { model.alertMessage = nil }
        } message: {
            Text(model.alertMessage ?? "")
        }
        .confirmationDialog(L(.confirmUninstallTitle), isPresented: $confirmingHelperUninstall) {
            Button(L(.uninstall), role: .destructive) {
                Task { await model.uninstallHelper() }
            }
            Button(L(.cancel), role: .cancel) {}
        } message: {
            Text(uninstallWarning)
        }
        .confirmationDialog(L(.confirmHelperUpdateTitle), isPresented: $confirmingHelperUpdate) {
            Button(L(.updateHelperButton)) {
                Task { await model.installHelper() }
            }
            Button(L(.cancel), role: .cancel) {}
        } message: {
            Text(L(.helperUpdateWhileRunningWarning))
        }
        .alert(L(.updateCheckTitle),
               isPresented: Binding(get: { model.updateCheckResult != nil },
                                    set: { if !$0 { model.updateCheckResult = nil } })) {
            if case .updateAvailable(let release) = model.updateCheckResult {
                Button(L(.openReleasePage)) { NSWorkspace.shared.open(release.pageURL) }
                Button(L(.ok), role: .cancel) {}
            } else {
                Button(L(.ok), role: .cancel) {}
            }
        } message: {
            Text(updateCheckDescription)
        }
    }

    // MARK: - Layout

    private var mainSplit: some View {
        NavigationSplitView {
            List(selection: Binding<Pane?>(get: { pane }, set: { pane = $0 ?? .overview })) {
                Section {
                    ForEach([Pane.overview, .device, .network, .activity]) { item in
                        sidebarRow(item)
                    }
                }
                Section {
                    sidebarRow(.settings)
                }
            }
            .navigationSplitViewColumnWidth(min: 190, ideal: 210, max: 260)
            .safeAreaInset(edge: .bottom) {
                SidebarStatus(model: model)
                    .padding(Design.Spacing.small)
            }
        } detail: {
            detail
                .navigationTitle(pane.title)
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        ConnectButton(model: model)
                    }
                }
        }
    }

    private func sidebarRow(_ item: Pane) -> some View {
        Label(item.title, systemImage: item.symbol)
            .badge(badge(for: item))
            .tag(item)
    }

    /// Sidebar badges point at things that need attention on that page.
    private func badge(for item: Pane) -> Text? {
        switch item {
        case .settings where model.helperVersionMismatch != nil || model.availableUpdate != nil:
            return Text(verbatim: "1")
        case .device where !model.devices.isEmpty && model.status.runState != .running:
            return Text(verbatim: "\(model.devices.count)")
        default:
            return nil
        }
    }

    @ViewBuilder
    private var detail: some View {
        switch pane {
        case .overview:
            Page {
                if let mismatch = model.helperVersionMismatch {
                    HelperMismatchBanner(mismatch: mismatch) { requestHelperUpdate() }
                }
                StatusHeroCard(model: model)
                EnvironmentWarningCard(environment: model.environment)
                ThroughputCard(model: model)
                NetworkSummaryCard(model: model) { pane = .network }
            }
        case .device:
            Page { DeviceCard(model: model) }
        case .network:
            Page { NetworkCard(model: model) }
        case .activity:
            LogCard(model: model)
                .padding(Design.Spacing.medium)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        case .settings:
            SettingsPage(model: model,
                         requestHelperUpdate: requestHelperUpdate,
                         requestHelperUninstall: { confirmingHelperUninstall = true })
        }
    }

    // MARK: - Actions & text

    private func requestHelperUpdate() {
        if model.status.runState == .running {
            confirmingHelperUpdate = true
        } else {
            Task { await model.installHelper() }
        }
    }

    private var updateCheckDescription: String {
        switch model.updateCheckResult {
        case .upToDate(let current):
            return L(.updateUpToDate, current)
        case .updateAvailable(let release):
            return L(.updateAvailable, release.version)
        case .failed(let reason):
            return L(.updateCheckFailed, reason)
        case .unavailable:
            return L(.updateDevBuild)
        case nil:
            return ""
        }
    }

    private var uninstallWarning: String {
        let base = L(.uninstallExplanation)
        return model.status.runState == .running
            ? L(.uninstallWhileRunningWarning) + base
            : base
    }
}

/// Scrollable, width-capped page column.
struct Page<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        ScrollView {
            VStack(spacing: Design.Spacing.gutter) {
                content()
            }
            .frame(maxWidth: Design.Window.contentMaxWidth)
            .padding(Design.Spacing.medium)
            .frame(maxWidth: .infinity)
        }
        .scrollBounceBehavior(.basedOnSize)
    }
}

/// Connect / Disconnect — the one primary action, always in the toolbar.
struct ConnectButton: View {
    @Bindable var model: AppModel

    var body: some View {
        let isRunning = model.status.runState == .running
        let isTransitional = model.status.runState.isTransitional

        Button {
            Task {
                if isRunning {
                    await model.stopSession()
                } else {
                    await model.startSession()
                }
            }
        } label: {
            HStack(spacing: Design.Spacing.tight) {
                if isTransitional || model.isBusy {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: isRunning ? "stop.fill" : "bolt.horizontal.fill")
                }
                Text(L(isRunning ? .disconnect : .connect))
            }
            .padding(.horizontal, 4)
        }
        .primaryActionButtonStyle()
        .tint(isRunning ? .red : .accentColor)
        .disabled(model.isBusy || isTransitional || (!isRunning && model.devices.isEmpty))
        .help(model.devices.isEmpty && !isRunning ? L(.connectDisabledHint) : "")
        .keyboardShortcut(.return, modifiers: .command)
    }
}

/// Compact live status at the bottom of the sidebar.
private struct SidebarStatus: View {
    var model: AppModel

    var body: some View {
        HStack(spacing: Design.Spacing.small) {
            Circle()
                .fill(Design.accent(for: model.status.runState))
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 1) {
                Text(Design.statusLabel(for: model.status))
                    .font(.callout.weight(.medium))
                    .lineLimit(1)
                if model.status.runState == .running {
                    Text("↓ \(Format.bitrate(model.throughput.receiveBitsPerSecond))  "
                         + "↑ \(Format.bitrate(model.throughput.transmitBitsPerSecond))")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, Design.Spacing.small)
        .padding(.vertical, Design.Spacing.tight)
        .glassSurface(cornerRadius: Design.Radius.control,
                      tint: model.status.runState == .running ? .green : nil)
    }
}

/// Shown on the overview when the running daemon predates this app.
private struct HelperMismatchBanner: View {
    let mismatch: HelperVersionMismatch
    let restart: () -> Void

    var body: some View {
        HStack(spacing: Design.Spacing.small) {
            Image(systemName: "arrow.triangle.2.circlepath")
                .foregroundStyle(.orange)
            Text(L(.helperVersionMismatch, mismatch.installed, mismatch.expected))
                .font(.callout)
            Spacer()
            Button(L(.updateHelperButton), action: restart)
                .secondaryActionButtonStyle()
                .help(L(.helperVersionMismatchTooltip, mismatch.installed, mismatch.expected))
        }
        .padding(Design.Spacing.small)
        .glassSurface(cornerRadius: Design.Radius.control, tint: .orange)
    }
}

/// Read-only network summary on the overview, linking to the Network page.
private struct NetworkSummaryCard: View {
    var model: AppModel
    let configure: () -> Void

    var body: some View {
        Card(title: L(.paneNetwork), systemImage: "network",
             accessory: AnyView(Button(L(.configureNetwork), action: configure)
                .buttonStyle(.link))) {
            if model.status.systemInterface.isEmpty {
                Text(L(.connectBeforeConfiguring))
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else {
                Grid(alignment: .leading, horizontalSpacing: Design.Spacing.large,
                     verticalSpacing: Design.Spacing.tight) {
                    row(L(.interfaceLabel), model.status.systemInterface)
                    row(L(.ipAddress), model.networkState.hasAddress
                        ? model.networkState.address : L(.notConfigured))
                    row(L(.router), model.networkState.router.isEmpty
                        ? "—" : model.networkState.router)
                    row("DNS", model.networkState.dnsServers.isEmpty
                        ? "—" : model.networkState.dnsServers.joined(separator: ", "))
                }
            }
        }
    }

    private func row(_ caption: String, _ value: String) -> some View {
        GridRow {
            Text(caption)
                .font(.callout)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
        }
    }
}
