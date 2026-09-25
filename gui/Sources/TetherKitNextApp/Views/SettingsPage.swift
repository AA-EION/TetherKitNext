import AppKit
import ServiceManagement
import SwiftUI
import TetherKitNextCore
import TetherKitNextIPC

/// Preference keys shared between views.
enum PreferenceKey {
    /// Show live rates next to the menu bar icon (upstream issue #1).
    static let menuBarShowsSpeed = "menuBarShowsSpeed"
    /// Existing key: `defaults write com.tetherkitnext.app updateCheckDisabled -bool YES`.
    static let updateCheckDisabled = "updateCheckDisabled"
}

/// Everything about the app itself, gathered in one place instead of
/// scattered across footers and the app menu.
struct SettingsPage: View {
    @Bindable var model: AppModel
    let requestHelperUpdate: () -> Void
    let requestHelperUninstall: () -> Void

    @AppStorage(PreferenceKey.menuBarShowsSpeed) private var menuBarShowsSpeed = true
    @AppStorage(PreferenceKey.updateCheckDisabled) private var updateCheckDisabled = false
    @AppStorage(AppModel.autoConfigureNetworkKey) private var autoConfigureNetwork = true
    @State private var launchAtLogin = SMAppService.mainApp.status == .enabled

    var body: some View {
        Form {
            backgroundSection
            commandLineSection
            generalSection
            updatesSection
            aboutSection
        }
        .formStyle(.grouped)
        .frame(maxWidth: Design.Window.contentMaxWidth)
        .frame(maxWidth: .infinity)
    }

    // MARK: Background component

    private var backgroundSection: some View {
        Section(L(.settingsBackgroundSection)) {
            LabeledContent {
                if let mismatch = model.helperVersionMismatch {
                    Text(L(.helperVersionMismatch, mismatch.installed, mismatch.expected))
                        .foregroundStyle(.orange)
                } else if case .available(let version) = model.helperAvailability {
                    Text(L(.backgroundRunning, HelperConstants.semanticVersion(of: version)))
                        .foregroundStyle(.secondary)
                }
            } label: {
                Label(L(.settingsBackgroundStatus), systemImage: "checkmark.shield")
            }

            HStack {
                Button(model.isBusy ? L(.installingProgress) : L(.updateHelperButton),
                       action: requestHelperUpdate)
                    .disabled(model.isBusy)
                    .help(L(.updateHelperDetail))
                Spacer()
                Button(L(.uninstallHelperMenuItem), role: .destructive,
                       action: requestHelperUninstall)
                    .disabled(model.isBusy)
            }
        }
    }

    // MARK: Command-line tool

    private var commandLineSection: some View {
        Section {
            LabeledContent {
                switch model.commandLineToolState {
                case .installed:
                    Button(L(.removeCommandLineTool)) {
                        Task { await model.setCommandLineToolInstalled(false) }
                    }
                    .disabled(model.isBusy)
                case .notInstalled:
                    Button(L(.installCommandLineTool)) {
                        Task { await model.setCommandLineToolInstalled(true) }
                    }
                    .primaryActionButtonStyle()
                    .disabled(model.isBusy)
                case .occupied:
                    EmptyView()
                }
            } label: {
                VStack(alignment: .leading, spacing: 2) {
                    Label(L(.commandLineToolTitle), systemImage: "terminal")
                    Text(commandLineStatus)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            if model.commandLineToolState == .installed {
                CopyableCommand(command: "tetherkitnext-cli --list")
                CopyableCommand(command: "sudo tetherkitnext-cli")
            }
        } header: {
            Text(L(.commandLineToolTitle))
        } footer: {
            Text(HelperConstants.commandLineToolLinkPath)
                .font(.caption.monospaced())
                .foregroundStyle(.tertiary)
        }
    }

    private var commandLineStatus: String {
        switch model.commandLineToolState {
        case .installed: return L(.commandLineToolInstalled)
        case .notInstalled: return L(.commandLineToolNotInstalled)
        case .occupied(let path): return L(.commandLineToolOccupied, path)
        }
    }

    // MARK: General

    private var generalSection: some View {
        Section(L(.settingsGeneralSection)) {
            LanguagePicker(model: model)
            Toggle(isOn: $autoConfigureNetwork) {
                Text(L(.autoConfigureNetwork))
                Text(L(.autoConfigureNetworkHelp))
            }
            Toggle(L(.menuBarShowSpeed), isOn: $menuBarShowsSpeed)
            Toggle(L(.launchAtLogin), isOn: $launchAtLogin)
                .onChange(of: launchAtLogin) { _, enabled in
                    setLaunchAtLogin(enabled)
                }
        }
    }

    private func setLaunchAtLogin(_ enabled: Bool) {
        let service = SMAppService.mainApp
        do {
            if enabled {
                try service.register()
            } else {
                try service.unregister()
            }
        } catch {
            model.alertMessage = L(.launchAtLoginFailed, error.localizedDescription)
        }
        let actual = service.status == .enabled
        if actual != launchAtLogin {
            launchAtLogin = actual
        }
    }

    // MARK: Updates

    private var updatesSection: some View {
        Section(L(.settingsUpdatesSection)) {
            Toggle(L(.autoCheckUpdates), isOn: Binding(get: { !updateCheckDisabled },
                                                        set: { updateCheckDisabled = !$0 }))
            LabeledContent {
                HStack {
                    if let update = model.availableUpdate {
                        Button(L(.helperUpdateBadge, update.version)) {
                            model.updateCheckResult = .updateAvailable(update)
                        }
                        .buttonStyle(.link)
                    }
                    Button(L(.checkNow)) {
                        Task { await model.checkForUpdates() }
                    }
                }
            } label: {
                Text(L(.currentVersion, UpdateChecker.currentVersion ?? "dev"))
            }
        }
    }

    // MARK: About

    private var aboutSection: some View {
        Section(L(.settingsAboutSection)) {
            Text(L(.aboutCredits))
                .font(.callout)
                .foregroundStyle(.secondary)
            LabeledContent(L(.madeByVendor)) {
                Link(L(.vendorWebsite), destination: Vendor.website)
            }
            LabeledContent("libtetherkitnext",
                           value: TetherKitNextLibrary.versionInfo.version)
            LabeledContent("libusb", value: TetherKitNextLibrary.versionInfo.libusb)
            HStack {
                if let url = UpdateChecker.projectURL {
                    Link(L(.projectWebsite), destination: url)
                }
                Spacer()
                Button(L(.showLicenses)) {
                    let licenses = Bundle.main.resourceURL?
                        .appendingPathComponent("Licenses", isDirectory: true)
                    if let licenses, FileManager.default.fileExists(atPath: licenses.path) {
                        NSWorkspace.shared.activateFileViewerSelecting([licenses])
                    }
                }
            }
        }
    }
}
