import SwiftUI
import TetherKitNextIPC

/// 设备选择与会话参数。
///
/// 会话跑起来之后整块变成只读 —— 改 MTU 或换设备都需要重连，做成「能改但不生效」
/// 比直接禁用更让人困惑。
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
            // 这三条正是 macOS 上 RNDIS 连不上的全部常见原因，按命中率排序。
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

    // 长解释一律放进悬停提示：这两个参数一年也调不了一次，说明文字却天天占着
    // 屏幕 —— 界面上只留「什么时候需要动它」这一句。
    private var tuningControls: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            HStack {
                Text("MTU")
                    .font(.callout)
                    .frame(width: 92, alignment: .leading)
                // 步进 100 而不是 1：MTU 是要和对端协商的，逐字节微调没有意义，
                // 而 1500 / 1400 / 2000 这样的整数才是用户真正会试的值。
                Stepper(value: $model.requestedMTU, in: 576...2048, step: 100) {
                    Text(L(.mtuBytes, String(model.requestedMTU)))
                        .font(.system(.callout, design: .monospaced))
                }
                .disabled(isLocked)
                Spacer()
            }
            // 上限只有在环境预检拿到之后才知道，所以是两条独立文案而不是拼接 ——
            // 「（本机上限 N）」这半句在英文里落到句子的另一个位置，拼不出来。
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

/// 设备列表里的一行。
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
                    // 这条信息对排障很有用：走了兜底路径说明设备的 CDC 描述符不
                    // 规范，日后若出问题这是第一个该看的线索。
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
