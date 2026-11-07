import SwiftUI
import TetherKitNextIPC
import TetherKitNextUIModel

/// Advanced → Interfaces: every network interface on the Mac with its
/// addresses and live traffic.
///
/// Read-only and unprivileged (getifaddrs + the routing socket's counters),
/// so it talks to neither the daemon nor the network. It is the first of the
/// opt-in advanced tools (docs/ADVANCED-TOOLS.md) and the base the planned
/// per-interface speed test builds on.
struct InterfacesPage: View {
    @State private var monitor = InterfaceMonitor()
    @AppStorage(PreferenceKey.interfacesShowInactive) private var showInactive = false

    var body: some View {
        Page {
            Card(title: L(.interfacesTitle), systemImage: "point.3.connected.trianglepath.dotted",
                 accessory: AnyView(Toggle(L(.interfacesShowInactive), isOn: $showInactive)
                    .toggleStyle(.checkbox)
                    .controlSize(.small))) {
                Text(L(.interfacesSubtitle))
                    .font(.callout)
                    .foregroundStyle(.secondary)

                let shown = monitor.interfaces.filter { showInactive || $0.isActive }
                if shown.isEmpty {
                    Text(L(.interfacesEmpty))
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.vertical, Design.Spacing.medium)
                } else {
                    VStack(spacing: 0) {
                        ForEach(Array(shown.enumerated()), id: \.element.id) { index, interface in
                            if index > 0 { Divider() }
                            InterfaceRow(interface: interface,
                                         rates: monitor.rates[interface.name] ?? .zero)
                        }
                    }
                }
            }
        }
        // Samples only while the page is on screen; .task cancels on leave.
        .task { await monitor.run() }
    }
}

private struct InterfaceRow: View {
    let interface: InterfaceSnapshot
    let rates: InterfaceRates

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: Design.Spacing.medium) {
            Circle()
                .fill(interface.isActive ? Color.green : Color.secondary.opacity(0.4))
                .frame(width: 8, height: 8)
                .accessibilityLabel(L(interface.isActive ? .interfaceActive : .interfaceInactive))

            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: interface.name)
                    .font(.body.monospaced().weight(.medium))
                if let link = interface.linkAddress {
                    Text(verbatim: link)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
            }
            .frame(width: 150, alignment: .leading)

            VStack(alignment: .leading, spacing: 2) {
                ForEach(interface.ipv4Addresses + interface.ipv6Addresses, id: \.self) { address in
                    Text(verbatim: address)
                        .font(.callout.monospaced())
                        .textSelection(.enabled)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                if interface.ipv4Addresses.isEmpty && interface.ipv6Addresses.isEmpty {
                    Text(L(.interfaceNoAddress))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            VStack(alignment: .trailing, spacing: 2) {
                Text(verbatim: "↓ \(Format.bitrate(rates.receiveBitsPerSecond))")
                Text(verbatim: "↑ \(Format.bitrate(rates.transmitBitsPerSecond))")
            }
            .font(.callout.monospacedDigit())
            .foregroundStyle(.secondary)
            .frame(width: 120, alignment: .trailing)
        }
        .padding(.vertical, Design.Spacing.small)
    }
}

/// Samples the interface list once a second.
@Observable
@MainActor
final class InterfaceMonitor {
    private(set) var interfaces: [InterfaceSnapshot] = []
    private(set) var rates: [String: InterfaceRates] = [:]

    @ObservationIgnored private var tracker = InterfaceRateTracker()
    @ObservationIgnored private let origin = ContinuousClock.now

    func run() async {
        while !Task.isCancelled {
            sample()
            try? await Task.sleep(for: .seconds(1))
        }
    }

    private func sample() {
        let snapshots = InterfaceSampler.snapshot()
        let elapsed = origin.duration(to: .now)
        let seconds = Double(elapsed.components.seconds)
            + Double(elapsed.components.attoseconds) / 1e18
        rates = tracker.update(with: snapshots, at: seconds)
        interfaces = snapshots
    }
}
