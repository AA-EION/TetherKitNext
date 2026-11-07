import AppKit
import SwiftUI
import TetherKitNextIPC
import TetherKitNextUIModel

struct ContentView: View {
    @Bindable var model: AppModel

    // @AppStorage rather than @SceneStorage: the window is an AppKit-owned
    // NSWindow (MainWindowController), which has no SwiftUI scene to store in.
    @AppStorage(PreferenceKey.selectedPane) private var storedPane = Pane.overview.rawValue
    @AppStorage(PreferenceKey.advancedToolsEnabled) private var advancedToolsEnabled = false
    @State private var confirmingHelperUninstall = false
    @State private var confirmingHelperUpdate = false

    /// The selected page. Resolution drops stale values (renamed pages, an
    /// advanced page after advanced tools were turned off) to the overview.
    private var pane: Pane {
        get { Pane.resolveStored(storedPane, advancedToolsEnabled: advancedToolsEnabled) }
        nonmutating set { storedPane = newValue.rawValue }
    }

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
                ForEach(Pane.sidebarSections(advancedToolsEnabled: advancedToolsEnabled),
                        id: \.self) { section in
                    if section.contains(where: \.isAdvanced) {
                        Section(L(.sidebarAdvancedSection)) {
                            ForEach(section) { item in sidebarRow(item) }
                        }
                    } else {
                        Section {
                            ForEach(section) { item in sidebarRow(item) }
                        }
                    }
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
                LinkKindCard(model: model)
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
        case .interfaces:
            InterfacesPage()
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
                Text(model.statusLabel)
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
