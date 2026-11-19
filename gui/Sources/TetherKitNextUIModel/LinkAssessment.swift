import Foundation
import TetherKitNextIPC

/// What kind of link the connected device is, judged from evidence alone.
///
/// Not every RNDIS device is a phone sharing its internet connection. Drones and
/// their remote controllers, FPV goggles, single-board computers in USB gadget
/// mode and cameras are *local links*: a private network to one device, often
/// without a DHCP server and never with a route to the internet. Treating those
/// as a failed phone connection is wrong and alarming.
///
/// ★ Deliberately not persisted ★
///
///   The assessment is recomputed from the current state on every poll and is
///   never stored per device. A label that stuck would misfire: a phone that is
///   slow to bring its upstream up, or slow to hand out a lease, would be
///   remembered as "local" and handled that way next time. Instead:
///   * Only facts the link itself reports count: a DHCP lease, whether it
///     names a router, the device's own address seen on the wire. Internet
///     reachability is never probed, so a phone whose Wi-Fi or mobile data is
///     slow or down is still "internet-capable" (it has a router) and is not
///     mistaken for a local device.
///   * A grace period covers slow DHCP servers before "no address" is said.
///   * Everything is re-evaluated live: a lease that arrives late flips the
///     assessment to `.internet` by itself, with no user action.
///   * Actions offered for local links (a temporary address) apply to the
///     current session only and never change the saved network settings.
public enum LinkAssessment: Equatable, Sendable {
    /// No session is running.
    case notConnected
    /// The user chose not to configure the interface ("Don't configure").
    case notConfigured
    /// Connected; waiting for DHCP within the grace period.
    case waitingForAddress
    /// A lease that names a router: the device shares a network with a way out
    /// (a phone, a router). Internet itself is not probed.
    case internet(address: String, router: String)
    /// An address but no router: a private link to the device itself.
    /// `selfAssigned` is a macOS link-local (169.254/16) fallback address.
    case localLink(address: String, peer: String?, selfAssigned: Bool)
    /// No address after the grace period. `peer` is the device's own address
    /// if it was seen on the wire, `suggestion` a host address next to it.
    case noAddress(peer: String?, suggestion: String?)

    /// How long DHCP may take before the UI stops saying "waiting".
    ///
    /// The daemon's own DHCP wait is 10 s, Android tethering first hands out a
    /// transitional 464XLAT lease, and slow devices take longer still; 20 s
    /// keeps a slow phone from being shown as "no address" too early.
    public static let gracePeriod: TimeInterval = 20

    public static func assess(isRunning: Bool,
                              connectedFor: TimeInterval,
                              mode: IPMode,
                              network: NetworkState,
                              peerAddress: String?) -> LinkAssessment {
        guard isRunning else { return .notConnected }
        if mode == IPMode.none && !network.hasAddress { return .notConfigured }

        let peer = peerAddress.flatMap { $0.isEmpty ? nil : $0 }
        if network.hasAddress {
            if !network.router.isEmpty {
                return .internet(address: network.address, router: network.router)
            }
            let selfAssigned = isLinkLocal(network.address)
            // macOS self-assigns 169.254.x.x while DHCP keeps retrying, so early
            // on that only means "no lease yet", not "no DHCP server".
            if selfAssigned && connectedFor < gracePeriod && mode == .dhcp {
                return .waitingForAddress
            }
            return .localLink(address: network.address, peer: peer, selfAssigned: selfAssigned)
        }

        if connectedFor < gracePeriod && mode == .dhcp {
            return .waitingForAddress
        }
        return .noAddress(peer: peer, suggestion: peer.flatMap(suggestedHostAddress(forPeer:)))
    }

    /// Whether routing all of the Mac's traffic through this link can work.
    /// "Route all traffic" is only honoured for links that name a router.
    public var canCarryInternet: Bool {
        if case .internet = self { return true }
        return false
    }

    public var isLocalOnly: Bool {
        switch self {
        case .localLink, .noAddress: return true
        default: return false
        }
    }

    // MARK: - Helpers

    static func isLinkLocal(_ address: String) -> Bool {
        address.hasPrefix("169.254.")
    }

    /// A host address in the device's /24 for a temporary manual setup.
    ///
    /// Picks the first of .10, .20, .100, .200 that is not the device itself.
    /// Not offered for link-local peers (macOS already self-assigns in
    /// 169.254/16) or anything that is not a plain IPv4 host address. macOS's
    /// IPConfiguration runs ARP conflict detection when the address is applied.
    public static func suggestedHostAddress(forPeer peer: String) -> String? {
        let parts = peer.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        let octets = parts.compactMap { UInt8($0) }
        guard octets.count == 4, octets[0] != 0, octets[0] != 127, octets[0] < 224,
              !(octets[0] == 169 && octets[1] == 254) else { return nil }
        for host: UInt8 in [10, 20, 100, 200] where host != octets[3] {
            return "\(octets[0]).\(octets[1]).\(octets[2]).\(host)"
        }
        return nil
    }
}
