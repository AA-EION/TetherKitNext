import SwiftUI
import TetherKitNextIPC
import TetherKitNextUIModel

/// Explains links that are not (yet) internet connections, on the overview.
///
/// Hidden for the normal phone case (a lease with a router) and when not
/// connected, so everyday tethering looks exactly as before. Shown for:
///   * waiting for DHCP (slow phones; within the grace period),
///   * local devices (drones and controllers, FPV goggles, boards in USB gadget
///     mode, cameras): an address but no router,
///   * no address at all: offers a session-only address next to the device's
///     own address when the bridge has seen it.
/// Everything here is live (`AppModel.linkAssessment`) and nothing is saved.
struct LinkKindCard: View {
    var model: AppModel
    @State private var addressInput = ""

    var body: some View {
        switch model.linkAssessment {
        case .notConnected, .notConfigured, .internet:
            EmptyView()

        case .waitingForAddress:
            Card(title: L(.linkWaitingTitle), systemImage: "hourglass") {
                HStack(spacing: Design.Spacing.small) {
                    ProgressView().controlSize(.small)
                    explanation(L(.linkWaitingBody))
                }
            }

        case .localLink(let address, let peer, let selfAssigned):
            Card(title: L(.linkLocalTitle), systemImage: "link") {
                VStack(alignment: .leading, spacing: Design.Spacing.small) {
                    explanation(L(.linkLocalBody))
                    addresses(mac: address, peer: peer)
                    if selfAssigned {
                        detail(L(.linkSelfAssignedNote))
                    }
                    routeAllNote
                    sessionOnlyControls
                    autoConfigureNote
                }
            }

        case .noAddress(let peer, let suggestion):
            Card(title: L(.linkNoAddressTitle), systemImage: "questionmark.circle") {
                VStack(alignment: .leading, spacing: Design.Spacing.small) {
                    explanation(L(.linkNoAddressBody))
                    addresses(mac: nil, peer: peer)
                    HStack(spacing: Design.Spacing.small) {
                        TextField(suggestion ?? L(.linkAddressPlaceholder), text: $addressInput)
                            .textFieldStyle(.roundedBorder)
                            .font(.body.monospaced())
                            .frame(maxWidth: 220)
                            .onSubmit { useAddress(fallback: suggestion) }
                        Button(L(.linkUseAddress)) { useAddress(fallback: suggestion) }
                            .disabled(model.isBusy || (trimmedInput.isEmpty && suggestion == nil))
                        Spacer()
                        Button(L(.linkRetryAutomatic)) {
                            Task { await model.retrySavedNetworkConfiguration() }
                        }
                        .disabled(model.isBusy)
                    }
                    detail(L(.linkNoAddressPhoneHint))
                    routeAllNote
                    autoConfigureNote
                }
            }
        }
    }

    // MARK: - Pieces

    private var trimmedInput: String {
        addressInput.trimmingCharacters(in: .whitespaces)
    }

    private func useAddress(fallback: String?) {
        let address = trimmedInput.isEmpty ? (fallback ?? "") : trimmedInput
        guard !address.isEmpty else { return }
        Task { await model.useSessionOnlyAddress(address) }
    }

    private func explanation(_ text: String) -> some View {
        Text(text)
            .font(.callout)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func detail(_ text: String) -> some View {
        Text(text)
            .font(.caption)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func addresses(mac: String?, peer: String?) -> some View {
        Grid(alignment: .leading, horizontalSpacing: Design.Spacing.large,
             verticalSpacing: Design.Spacing.tight) {
            if let mac {
                GridRow {
                    Text(L(.linkThisMac)).foregroundStyle(.secondary)
                    Text(verbatim: mac).font(.body.monospaced()).textSelection(.enabled)
                }
            }
            GridRow {
                Text(L(.linkDevice)).foregroundStyle(.secondary)
                if let peer {
                    Text(verbatim: peer).font(.body.monospaced()).textSelection(.enabled)
                } else {
                    Text(L(.linkPeerUnknown)).foregroundStyle(.secondary)
                }
            }
        }
        .font(.callout)
    }

    @ViewBuilder
    private var routeAllNote: some View {
        if model.networkConfiguration.setDefaultRoute {
            Label(L(.linkRouteAllSkipped), systemImage: "info.circle")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private var sessionOnlyControls: some View {
        if model.sessionOnlyAddress != nil {
            HStack {
                detail(L(.linkSessionOnlyNote))
                Spacer()
                Button(L(.linkRetryAutomatic)) {
                    Task { await model.retrySavedNetworkConfiguration() }
                }
                .disabled(model.isBusy)
            }
        }
    }

    @ViewBuilder
    private var autoConfigureNote: some View {
        if let note = model.autoConfigureNote {
            Text(note)
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}
