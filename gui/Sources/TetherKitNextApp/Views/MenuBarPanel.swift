import AppKit
import SwiftUI
import TetherKitNextIPC

/// The resident label in the menu bar: status icon + real-time rate.
///
/// The rate text is shown only while a session is running -- a string of "0K" when idle is just noise, and the icon itself already makes the
/// state clear. Digits are monospaced, to avoid pushing other menu bar items on the right around when the rate jumps.
struct MenuBarLabel: View {
    var model: AppModel
    @AppStorage(PreferenceKey.menuBarShowsSpeed) private var showsSpeed = true

    var body: some View {
        HStack(spacing: 2) {
            Image(systemName: Design.statusSymbol(for: model.status))
            if model.status.runState == .running, showsSpeed {
                // Fully monospaced + fixed-width RateFormat: the status item
                // keeps one width whatever the rate (upstream issue #1).
                Text(speedText)
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
            }
        }
    }

    private var speedText: String {
        "↓\(Format.compactBitrate(model.throughput.receiveBitsPerSecond))"
            + " ↑\(Format.compactBitrate(model.throughput.transmitBitsPerSecond))"
    }
}

/// The small panel after clicking open a menu bar item.
///
/// Content is chosen by "what you need to know at a glance": status, rate, NIC and address, plus two actions
/// (open the main window, quit). Configuration, charts and logs all stay in the main window -- the panel is not a second UI.
struct MenuBarPanel: View {
    var model: AppModel
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismiss) private var dismiss

    private var status: SessionStatus { model.status }

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            header

            switch status.runState {
            case .running:
                runningDetails
            case .failed:
                if !status.fatalMessage.isEmpty {
                    Text(status.fatalMessage)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .lineLimit(3)
                        .fixedSize(horizontal: false, vertical: true)
                }
            default:
                idleHint
            }

            Divider()
            ConnectButton(model: model)
                .controlSize(.large)
                .frame(maxWidth: .infinity)
            actions
        }
        .padding(Design.Spacing.medium)
        .frame(width: 280)
        // Same reason as the main window: messages come from a global table lookup, and changing the language will not make any @Observable property
        // "appear" to change, so an explicit identity is needed to force a rebuild. The panel itself refreshes with polling,
        // and without this line the language would also catch up in the next period -- but when changing the language from the panel, the user is staring at
        // this very panel, and lagging a beat would be taken as not having taken effect.
        .id(model.languageRevision)
    }

    private var header: some View {
        HStack(spacing: Design.Spacing.tight) {
            Circle()
                .fill(Design.accent(for: status.runState))
                .frame(width: 8, height: 8)
            Text(Design.statusLabel(for: status))
                .font(.headline)
            Spacer()
            if let duration = model.connectedDuration {
                Text(Format.duration(duration))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private var runningDetails: some View {
        speedRow(symbol: "arrow.down", caption: L(.downstreamShort),
                 bitsPerSecond: model.throughput.receiveBitsPerSecond, tint: .blue)
        speedRow(symbol: "arrow.up", caption: L(.upstreamShort),
                 bitsPerSecond: model.throughput.transmitBitsPerSecond, tint: .purple)

        if !status.systemInterface.isEmpty {
            Text("\(status.systemInterface) · "
                 + (model.networkState.hasAddress ? model.networkState.address
                                                  : L(.noIPConfigured)))
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
        }
    }

    private var idleHint: some View {
        Text(model.devices.isEmpty
             ? L(.menuBarNoDevice)
             : L(.menuBarReady, model.devices.first?.displayName ?? ""))
            .font(.caption)
            .foregroundStyle(.secondary)
            .lineLimit(2)
            .fixedSize(horizontal: false, vertical: true)
    }

    private var actions: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.tight) {
            HStack {
                Button(L(.openMainWindow)) {
                    dismiss()
                    presentMainWindow(model, openWindow)
                }
                .secondaryActionButtonStyle()
                .controlSize(.small)

                Spacer()

                // In menu-bar-only mode this panel is the sole entry point, and the language switch must be reachable here too --
                // otherwise a user who cannot understand the UI would first have to know "go to the App menu" to change the language,
                // and in this mode the App menu is visible only when the window is opened.
                languageMenu

                Button(L(.quit)) {
                    NSApp.terminate(nil)
                }
                .controlSize(.small)
            }

            if status.runState == .running {
                // The session belongs to the helper, and quitting the App does not affect it -- this must be said out loud,
                // otherwise users would think "quit = disconnect" and not dare to click.
                Text(L(.quitTooltip))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    /// The language switch. Options are written as native-language names and are not translated with the UI language -- see the explanation of LanguageMenu.
    private var languageMenu: some View {
        Menu {
            LanguagePicker(model: model)
                .pickerStyle(.inline)
                .labelsHidden()
        } label: {
            Image(systemName: "globe")
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help(L(.languageLabel))
    }

    private func speedRow(symbol: String, caption: String, bitsPerSecond: Double,
                          tint: Color) -> some View {
        HStack(spacing: Design.Spacing.small) {
            Image(systemName: symbol)
                .font(.caption)
                .foregroundStyle(tint)
                .frame(width: 14)
            Text(caption)
                .font(.callout)
                .foregroundStyle(.secondary)
            Spacer()
            Text(Format.bitrate(bitsPerSecond))
                .font(.callout.monospacedDigit())
                .foregroundStyle(tint)
        }
    }
}
