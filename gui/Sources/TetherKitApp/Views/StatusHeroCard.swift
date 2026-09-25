import SwiftUI
import TetherKitIPC

/// 主状态卡：整个界面的视觉与操作重心。
///
/// 一屏之内回答三个问题：现在通不通、通到哪张网卡、什么 IP。
/// 其余细节都往下面的卡片里放 —— 用户九成的时间只看这一块。
struct StatusHeroCard: View {
    @Bindable var model: AppModel

    private var accent: Color { Design.accent(for: model.status.runState) }

    var body: some View {
        // The hero is status chrome, not content: it floats on Liquid Glass
        // tinted with the connection state. Connect/Disconnect lives in the
        // toolbar (ConnectButton) so it is reachable from every page.
        HStack(alignment: .center, spacing: Design.Spacing.large) {
            StatusRing(status: model.status, accent: accent)

            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                headline
                subline
                addressLine
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(Design.Spacing.large)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface(cornerRadius: Design.Radius.hero,
                      tint: model.status.runState == .idle ? nil : accent)
    }

    private var headline: some View {
        HStack(spacing: Design.Spacing.small) {
            Text(Design.statusLabel(for: model.status))
                .font(.system(.title2, design: .rounded).weight(.semibold))

            if model.status.runState == .running {
                StatusBadge(text: L(model.status.linkUp ? .linkUp : .linkDown),
                            color: model.status.linkUp ? .green : .orange)
            }
            if model.status.paused {
                StatusBadge(text: L(.transferPaused), color: .orange)
            }
        }
    }

    @ViewBuilder
    private var subline: some View {
        if model.status.runState == .failed, !model.status.fatalMessage.isEmpty {
            Text(model.status.fatalMessage)
                .font(.callout)
                .foregroundStyle(.red)
                .fixedSize(horizontal: false, vertical: true)
                .textSelection(.enabled)
        } else if !model.status.deviceDescription.isEmpty {
            Text(deviceSummary)
                .font(.callout)
                .foregroundStyle(.secondary)
        } else if let device = model.selectedDevice {
            Text(L(.pendingDevice, device.displayName))
                .font(.callout)
                .foregroundStyle(.secondary)
        } else {
            Text(L(.noRNDISDeviceDetected))
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var deviceSummary: String {
        var parts: [String] = []
        if !model.status.vendorDescription.isEmpty {
            parts.append(model.status.vendorDescription)
        }
        parts.append(model.status.deviceDescription)
        if model.status.mtu > 0 {
            parts.append("MTU \(model.status.mtu)")
        }
        if model.status.linkSpeedMbps > 0 {
            parts.append("\(model.status.linkSpeedMbps) Mbps")
        }
        return parts.joined(separator: " · ")
    }

    @ViewBuilder
    private var addressLine: some View {
        if model.status.runState == .running || model.status.runState == .starting {
            HStack(spacing: Design.Spacing.large) {
                inlineMetric(L(.interfaceLabel), model.status.systemInterface.isEmpty
                    ? L(.interfaceCreating) : model.status.systemInterface)
                inlineMetric(L(.ipAddress), model.networkState.hasAddress
                    ? model.networkState.address : L(.notConfigured))
                if let duration = model.connectedDuration {
                    inlineMetric(L(.statusConnected), Format.duration(duration))
                }
            }
        }
    }

    private func inlineMetric(_ caption: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(caption)
                .font(.caption2)
                .foregroundStyle(.tertiary)
            Text(value)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
        }
    }
}

/// 状态圆环。
///
/// 运行中时外圈缓慢旋转、内圈呼吸 —— 这是界面上唯一的动效，用来表达「它在活着
/// 干活」。其余状态一律静止，避免把注意力浪费在没有信息量的动画上。
private struct StatusRing: View {
    let status: SessionStatus
    let accent: Color

    @State private var rotation: Double = 0
    @State private var breathing = false

    private var isActive: Bool { status.runState == .running && status.linkUp }
    private var isWorking: Bool { status.runState.isTransitional }

    var body: some View {
        ZStack {
            Circle()
                .fill(accent.opacity(0.12))
                .frame(width: 64, height: 64)
                .scaleEffect(breathing && isActive ? 1.08 : 1.0)

            Circle()
                .stroke(accent.opacity(0.25), lineWidth: 3)
                .frame(width: 64, height: 64)

            // 过渡态用一段旋转的弧表示「正在进行中，进度不可知」。
            Circle()
                .trim(from: 0, to: isWorking ? 0.25 : (isActive ? 1.0 : 0.0))
                .stroke(accent, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                .frame(width: 64, height: 64)
                .rotationEffect(.degrees(isWorking ? rotation : -90))

            Image(systemName: Design.statusSymbol(for: status))
                .font(.system(size: 22, weight: .medium))
                .foregroundStyle(accent)
                .symbolRenderingMode(.hierarchical)
        }
        .frame(width: 72, height: 72)
        .onAppear { restartAnimations() }
        .onChange(of: status.runState) { _, _ in restartAnimations() }
        .onChange(of: status.linkUp) { _, _ in restartAnimations() }
    }

    private func restartAnimations() {
        if isWorking {
            withAnimation(.linear(duration: 1.1).repeatForever(autoreverses: false)) {
                rotation = 360
            }
        } else {
            rotation = 0
        }
        withAnimation(.easeInOut(duration: 1.8).repeatForever(autoreverses: true)) {
            breathing = isActive
        }
    }
}
