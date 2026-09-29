import SwiftUI
import TetherKitNextCore
import TetherKitNextIPC

/// Connectivity method configuration: DHCP or static IP.
///
/// * Why the "currently in effect" column exists separately *
///   It shows the state **read back from the system**, not the value we applied. The two may differ --
///   the most typical is DNS in static mode: IPConfiguration publishes DNS only in DHCP mode,
///   and in static mode all we can do is best-effort add a key, and whether the system adopts it depends on IPMonitor.
///   Rather than promise the user a result that may not hold, it is better to lay out the real state.
///
/// The height of this card directly determines whether the left column fits on one screen (the tallest state is "static form + currently in effect"
/// both expanded), so the input cells and readback values are all arranged in two columns, and long explanations all go into hover tooltips.
struct NetworkCard: View {
    @Bindable var model: AppModel

    /// Disable the whole block when the NIC has not been created yet -- there is no NIC to configure, and letting the user fill in and then report an error is the worst order.
    private var interfaceReady: Bool { !model.status.systemInterface.isEmpty }

    private var canAddDNS: Bool {
        // The upper limit matches the C ABI's TK_DNS_MAX; extra ones would be dropped, so it is better simply not to let them be added.
        model.networkConfiguration.dnsServers.count < 4
    }

    /// The hover explanation of static DNS. It is information at the "when you need to care" level,
    /// and does not deserve a permanent row.
    /// A computed property rather than `static let`: the latter is evaluated only once and would no longer update after switching the language.
    private static var dnsHint: String { L(.dnsEffectivenessTooltip) }

    var body: some View {
        Card(title: L(.ipModeLabel), systemImage: "network", accessory: AnyView(interfaceBadge)) {
            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                modePicker

                switch model.networkConfiguration.mode {
                case .dhcp:
                    dhcpExplanation
                case .manual:
                    manualForm
                case .none:
                    EmptyView()
                }

                defaultRouteToggle
                actionRow

                if interfaceReady {
                    Divider()
                    EffectiveStateView(state: model.networkState,
                                       interface: model.status.systemInterface)
                }
            }
            .disabled(!interfaceReady)
            .opacity(interfaceReady ? 1 : 0.55)
            .overlay(alignment: .center) {
                if !interfaceReady {
                    Text(L(.connectBeforeConfiguring))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .padding(Design.Spacing.small)
                        .background(.regularMaterial, in: Capsule())
                }
            }
        }
    }

    @ViewBuilder
    private var interfaceBadge: some View {
        if interfaceReady {
            StatusBadge(text: model.status.systemInterface, color: .accentColor)
        }
    }

    private var modePicker: some View {
        Picker(L(.ipModeLabel), selection: $model.networkConfiguration.mode) {
            // Deliberately not putting "do not configure" in the picker: it is an action (revoke), not a connectivity
            // method. Mixing them would make people think choosing it already takes effect.
            Text(IPMode.dhcp.displayName).tag(IPMode.dhcp)
            Text(IPMode.manual.displayName).tag(IPMode.manual)
        }
        .pickerStyle(.segmented)
        .labelsHidden()
    }

    private var dhcpExplanation: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.tight) {
            Label(L(.dhcpHelp), systemImage: "wand.and.stars")
                .font(.callout)
            Text(L(.dhcpTooltip))
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// The static form. The four address cells are arranged in a two-column grid -- IPv4 is short, half-column width is enough,
    /// and the height is directly halved.
    private var manualForm: some View {
        Grid(alignment: .leading,
             horizontalSpacing: Design.Spacing.medium,
             verticalSpacing: Design.Spacing.small) {
            GridRow {
                AddressField(label: L(.ipAddress),
                             placeholder: "192.168.42.100",
                             text: $model.networkConfiguration.address,
                             isValid: NetworkValidator.isValidIPv4(model.networkConfiguration.address))
                AddressField(label: L(.netmask),
                             placeholder: "255.255.255.0",
                             text: $model.networkConfiguration.netmask,
                             isValid: NetworkValidator.isValidNetmask(model.networkConfiguration.netmask))
            }
            GridRow {
                AddressField(label: L(.router),
                             placeholder: L(.routerOptional),
                             text: $model.networkConfiguration.router,
                             isValid: model.networkConfiguration.router.isEmpty
                                 || NetworkValidator.isValidIPv4(model.networkConfiguration.router))
                if model.networkConfiguration.dnsServers.isEmpty {
                    emptyDNSCell
                } else {
                    dnsField(at: 0)
                }
            }
            // DNS entries from the 2nd on each occupy the right cell of a row, with the left cell empty -- aligned with the DNS above.
            ForEach(Array(model.networkConfiguration.dnsServers.indices.dropFirst()),
                    id: \.self) { index in
                GridRow {
                    Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                    dnsField(at: index)
                }
            }
        }
    }

    /// One DNS entry: input cell + delete, and the last one also carries an "Add".
    private func dnsField(at index: Int) -> some View {
        HStack(spacing: Design.Spacing.tight) {
            AddressField(label: index == 0 ? "DNS" : "",
                         placeholder: "223.5.5.5",
                         text: $model.networkConfiguration.dnsServers[index],
                         isValid: model.networkConfiguration.dnsServers[index].isEmpty
                             || NetworkValidator.isValidIPv4(model.networkConfiguration.dnsServers[index]))

            Button {
                model.networkConfiguration.dnsServers.remove(at: index)
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .help(L(.deleteThisEntry))

            if index == model.networkConfiguration.dnsServers.count - 1, canAddDNS {
                Button {
                    model.networkConfiguration.dnsServers.append("")
                } label: {
                    Image(systemName: "plus.circle")
                }
                .buttonStyle(.borderless)
                .help(L(.addDNSServer))
            }
        }
        .help(Self.dnsHint)
    }

    /// The "Add" that holds the cell when there is not a single DNS entry.
    private var emptyDNSCell: some View {
        HStack(spacing: Design.Spacing.tight) {
            Text("DNS")
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(width: AddressField.labelWidth, alignment: .leading)
            Button {
                model.networkConfiguration.dnsServers.append("")
            } label: {
                Label(L(.add), systemImage: "plus.circle")
            }
            .buttonStyle(.borderless)
            .font(.callout)
            Spacer(minLength: 0)
        }
        .help(Self.dnsHint)
    }

    private var defaultRouteToggle: some View {
        Toggle(isOn: $model.networkConfiguration.setDefaultRoute) {
            VStack(alignment: .leading, spacing: 1) {
                Text(L(.setDefaultRoute))
                Text(L(.setDefaultRouteHelp))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .help(L(.setDefaultRouteTooltip))
    }

    private var actionRow: some View {
        HStack(spacing: Design.Spacing.small) {
            Button {
                Task { await model.applyNetworkConfiguration() }
            } label: {
                HStack(spacing: Design.Spacing.tight) {
                    if model.isBusy {
                        ProgressView().controlSize(.small)
                    }
                    Text(L(.apply))
                }
                .frame(minWidth: 56)
            }
            .buttonStyle(.borderedProminent)
            .disabled(model.isBusy)

            Button(L(.clearConfiguration)) {
                Task { await model.clearNetworkConfiguration() }
            }
            .disabled(model.isBusy || !model.networkState.hasAddress)
            .help(L(.clearConfigurationTooltip))

            Spacer()
        }
    }
}

/// A compact address input box with immediate validation feedback: the label on the left, and the validation icon overlaid on the inner side of the input box
/// (every bit of width is precious in a two-column layout).
///
/// Validation is done while typing rather than at submit: a wrongly entered address is the most common operation mistake, and reporting an error only after clicking "Apply"
/// would make the user guess back and forth which cell is wrong.
private struct AddressField: View {
    /// Label column width. The labels of the readback area use it too, so the vertical alignment lines of the two blocks are the same one.
    static let labelWidth: CGFloat = 56

    let label: String
    let placeholder: String
    @Binding var text: String
    let isValid: Bool

    var body: some View {
        HStack(spacing: Design.Spacing.tight) {
            Text(label)
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(width: Self.labelWidth, alignment: .leading)

            TextField(placeholder, text: $text)
                .textFieldStyle(.roundedBorder)
                .font(.system(.callout, design: .monospaced))
                .overlay(alignment: .trailing) {
                    // Empty input is not marked red: not finished filling in yet does not count as wrong.
                    if !text.isEmpty {
                        Image(systemName: isValid ? "checkmark.circle.fill"
                                                  : "exclamationmark.circle.fill")
                            .foregroundStyle(isValid ? Color.green : Color.orange)
                            .font(.caption)
                            .padding(.trailing, 4)
                            .allowsHitTesting(false)
                    }
                }
        }
    }
}

/// The real in-effect state read back from the system.
private struct EffectiveStateView: View {
    let state: NetworkState
    let interface: String

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.tight) {
            HStack(spacing: Design.Spacing.small) {
                Text(L(.currentlyEffective))
                    .font(.subheadline.weight(.medium))
                if !state.method.isEmpty {
                    StatusBadge(text: state.method, color: .accentColor)
                }
                if !state.serviceState.isEmpty {
                    StatusBadge(text: state.serviceState,
                                color: state.serviceState == "BOUND" ? .green : .orange)
                }
                if state.isPrimaryDefaultRoute {
                    StatusBadge(text: L(.primaryDefaultRoute), color: .green)
                }
            }

            if state.hasAddress {
                // Two columns of four cells rather than four rows: this is a pure display area, and compactness comes first. Values that do not fit
                // (multiple DNS) are truncated in the middle for display, and hovering shows the whole, and can also be selected and copied.
                Grid(alignment: .leading,
                     horizontalSpacing: Design.Spacing.medium,
                     verticalSpacing: Design.Spacing.tight) {
                    GridRow {
                        readbackCell(L(.ipAddress), state.address)
                        readbackCell(L(.netmask), state.netmask)
                    }
                    GridRow {
                        readbackCell(L(.router), state.router)
                        readbackCell("DNS", state.dnsServers.isEmpty
                                     ? L(.notEffective)
                                     : state.dnsServers.joined(separator: L(.listSeparator)))
                    }
                }
            } else {
                Text(L(.noAddressYet, interface))
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func readbackCell(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: Design.Spacing.tight) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(width: AddressField.labelWidth, alignment: .leading)
            Text(value.isEmpty ? "—" : value)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .help(value)
    }
}
