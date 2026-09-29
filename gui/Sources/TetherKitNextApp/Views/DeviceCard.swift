import SwiftUI
import TetherKitNextIPC

/// Device selection and session parameters.
///
/// Once the session is running the whole block becomes read-only -- changing the MTU or switching devices both require reconnecting, and making it "changeable but ineffective"
/// would be more confusing than simply disabling it.
struct DeviceCard: View {
    @Bindable var model: AppModel

    private var isLocked: Bool {
        model.status.runState == .running || model.status.runState.isTransitional
    }

    var body: some View {
        Card(title: L(.usbDeviceSectionTitle),
             systemImage: "cable.connector",
             accessory: AnyView(refreshButton)) {
            if model.devices.isEmpty {
                emptyState
            } else {
                VStack(alignment: .leading, spacing: Design.Spacing.medium) {
                    devicePicker
                    Divider()
                    tuningControls
                }
            }
        }
    }

    private var refreshButton: some View {
        Button {
            Task { await model.refreshDevices() }
        } label: {
            Image(systemName: "arrow.clockwise")
        }
        .buttonStyle(.borderless)
        .disabled(isLocked)
        .help(L(isLocked ? .cannotRescanWhileRunning : .rescanUSBDevices))
    }

    private var emptyState: some View {
        ContentUnavailableView {
            Label(L(.noDeviceDetected), systemImage: "cable.connector.slash")
        } description: {
            // These three are exactly all the common reasons RNDIS cannot connect on macOS, ordered by hit rate.
            VStack(alignment: .leading, spacing: 4) {
                Text(L(.deviceChecklistCable))
                Text(L(.deviceChecklistTethering))
                Text(L(.deviceChecklistUnlocked))
            }
            .font(.callout)
        }
        .frame(maxWidth: .infinity)
    }

    private var devicePicker: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            ForEach(model.devices) { device in
                DeviceRow(device: device,
                          isSelected: (model.selectedDevice?.id == device.id),
                          isLocked: isLocked) {
                    model.selectedDeviceID = device.id
                }
            }
        }
    }

    // Long explanations always go into hover tooltips: these two parameters cannot be tuned once a year, yet explanatory text occupies
    // the screen every day -- only the single sentence "when you need to touch it" is left on the UI.
    private var tuningControls: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            HStack {
                Text("MTU")
                    .font(.callout)
                    .frame(width: 92, alignment: .leading)
                // Step by 100 rather than 1: the MTU must be negotiated with the peer, byte-by-byte fine-tuning is meaningless,
                // and round numbers like 1500 / 1400 / 2000 are the values users will really try.
                Stepper(value: $model.requestedMTU, in: 576...2048, step: 100) {
                    Text(L(.mtuBytes, String(model.requestedMTU)))
                        .font(.system(.callout, design: .monospaced))
                }
                .disabled(isLocked)
                Spacer()
            }
            // The upper limit is known only after the environment preflight, so these are two independent messages rather than concatenation --
            // the half sentence "(this machine's limit N)" lands in another position in the English sentence and cannot be concatenated.
            Text(model.environment.map { L(.mtuHelpWithLimit, Int($0.fethMaxMTU)) }
                 ?? L(.mtuHelp))
                .font(.caption)
                .foregroundStyle(.secondary)
                .help(L(.mtuTooltip))

            Toggle(isOn: $model.adoptDeviceMAC) {
                VStack(alignment: .leading, spacing: 1) {
                    Text(L(.adoptDeviceMAC))
                    Text(L(.adoptDeviceMACHelp))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .disabled(isLocked)
            .help(L(.adoptDeviceMACTooltip))
            .padding(.top, 2)
        }
    }
}

/// One row in the device list.
private struct DeviceRow: View {
    let device: DeviceDescriptor
    let isSelected: Bool
    let isLocked: Bool
    let onSelect: () -> Void

    var body: some View {
        Button(action: onSelect) {
            HStack(spacing: Design.Spacing.small) {
                Image(systemName: isSelected ? "largecircle.fill.circle" : "circle")
                    .foregroundStyle(isSelected ? Color.accentColor : Color.secondary)

                VStack(alignment: .leading, spacing: 1) {
                    Text(device.displayName)
                        .font(.callout)
                        .foregroundStyle(.primary)
                    HStack(spacing: Design.Spacing.tight) {
                        Text(device.summary)
                            .font(.system(.caption, design: .monospaced))
                        if !device.serial.isEmpty {
                            Text("SN \(device.serial)")
                                .font(.system(.caption, design: .monospaced))
                        }
                    }
                    .foregroundStyle(.secondary)
                }

                Spacer(minLength: 0)

                if device.usedAndroidQuirk {
                    // This information is very useful for troubleshooting: taking the fallback path indicates the device's CDC descriptor is not
                    // standard, and if problems arise later this is the first clue to look at.
                    StatusBadge(text: L(.compatibilityMode), color: .orange)
                        .help(L(.compatibilityModeTooltip))
                }
            }
            .padding(Design.Spacing.small)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(isSelected ? Color.accentColor.opacity(0.10) : Color.clear,
                        in: RoundedRectangle(cornerRadius: Design.Radius.control))
            .overlay(
                RoundedRectangle(cornerRadius: Design.Radius.control)
                    .strokeBorder(isSelected ? Color.accentColor.opacity(0.4) : .clear,
                                  lineWidth: 1))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(isLocked)
    }
}
