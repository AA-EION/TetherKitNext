import CTetherKitNext
import Foundation
import TetherKitNextIPC

/// Connectivity method configuration of the virtual NIC.
///
/// `apply` / `clear` need root (called only inside the helper); `query` does not.
public enum NetworkConfigurator {
    /// The capacity of each address field in tk_ip_config_t, used to locate the rows of the two-dimensional array `dns[4][46]`.
    private static let addressStride = Int(TK_ADDRESS_CAPACITY)
    private static let maxDNSServers = Int(TK_DNS_MAX)

    /// Applies the configuration.
    ///
    /// WARNING: In DHCP mode it **blocks** until a lease is obtained or it times out (the library's internal cap is 10 seconds). This is deliberate:
    /// "applied, go poll it yourself" cannot give meaningful success/failure feedback to the UI. The caller must
    /// guarantee it is not called on the main thread.
    public static func apply(_ configuration: NetworkConfiguration, to interface: String) throws {
        var raw = tk_ip_config_t()
        tk_ip_config_init(&raw)
        raw.mode = configuration.mode.rawValue
        raw.set_default_route = configuration.setDefaultRoute

        if configuration.mode == .manual {
            setFixedCArray(&raw.address, to: configuration.address)
            setFixedCArray(&raw.netmask, to: configuration.netmask)
            setFixedCArray(&raw.router, to: configuration.router)

            let servers = configuration.dnsServers
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
                .prefix(maxDNSServers)
            for (index, server) in servers.enumerated() {
                setFixedCArrayRow(&raw.dns, row: index, stride: addressStride, to: server)
            }
            raw.dns_count = Int32(servers.count)
        }

        var error = tk_error_t()
        let result = interface.withCString { tk_net_apply($0, &raw, &error) }
        try check(result, error)
    }

    /// Revokes the IP configuration on the NIC.
    public static func clear(interface: String) throws {
        var error = tk_error_t()
        let result = interface.withCString { tk_net_clear($0, &error) }
        try check(result, error)
    }

    /// Reads back the state the NIC **actually has in effect**.
    ///
    /// When the NIC does not exist yet it returns an all-empty state rather than throwing -- the UI also refreshes when a session has not started,
    /// and showing a false error then would only mislead people.
    public static func query(interface: String) throws -> NetworkState {
        var raw = tk_net_state_t()
        var error = tk_error_t()
        let result = interface.withCString { tk_net_query($0, &raw, &error) }
        try check(result, error)

        var servers: [String] = []
        for index in 0..<Int(max(0, min(raw.dns_count, Int32(maxDNSServers)))) {
            let server = fixedCArrayRow(raw.dns, row: index, stride: addressStride)
            if !server.isEmpty { servers.append(server) }
        }

        return NetworkState(
            hasAddress: raw.has_address,
            address: String(fixedCArray: raw.address),
            netmask: String(fixedCArray: raw.netmask),
            router: String(fixedCArray: raw.router),
            dnsServers: servers,
            method: String(fixedCArray: raw.method),
            serviceState: String(fixedCArray: raw.service_state),
            hasDefaultRoute: raw.has_default_route,
            isPrimaryDefaultRoute: raw.is_primary_default_route)
    }
}

// MARK: - Validation

/// Input validation of the static IP form.
///
/// Placed in the shared layer rather than in the UI: the helper side must also put up a barrier (XPC is reachable by any local process),
/// and using the same rules on both sides avoids the split of "the UI passed but the helper rejected".
public enum NetworkValidator {
    /// Whether it is a legal dotted-decimal IPv4 address.
    public static func isValidIPv4(_ text: String) -> Bool {
        var address = in_addr()
        return text.withCString { inet_pton(AF_INET, $0, &address) } == 1
    }

    /// Whether it is a legal subnet mask.
    ///
    /// Stricter than "is a legal IPv4": the mask in binary must be consecutive 1s followed by consecutive 0s.
    /// Something like 255.255.0.255 passes inet_pton but is wrong as a mask, and the kernel's reaction to it
    /// is to produce a weird route that is very hard to track down -- so it is blocked at the entrance.
    public static func isValidNetmask(_ text: String) -> Bool {
        var address = in_addr()
        guard text.withCString({ inet_pton(AF_INET, $0, &address) }) == 1 else { return false }
        let host = UInt32(bigEndian: address.s_addr)
        // The necessary and sufficient condition for a contiguous mask: after inverting and adding one it is a power of 2 (or 0, corresponding to 255.255.255.255).
        let inverted = ~host
        return inverted & (inverted &+ 1) == 0
    }

    /// Validates a static IP configuration item by item, returning the description of the first one that fails; returns nil when all pass.
    public static func validationMessage(for configuration: NetworkConfiguration) -> String? {
        guard configuration.mode == .manual else { return nil }

        if !isValidIPv4(configuration.address) {
            return L(.invalidIPAddress)
        }
        if !isValidNetmask(configuration.netmask) {
            return L(.invalidNetmask)
        }
        let router = configuration.router.trimmingCharacters(in: .whitespaces)
        if !router.isEmpty, !isValidIPv4(router) {
            return L(.invalidRouter)
        }
        if configuration.setDefaultRoute, router.isEmpty {
            return L(.routerRequiredForDefaultRoute)
        }
        for server in configuration.dnsServers where !server.trimmingCharacters(in: .whitespaces).isEmpty {
            if !isValidIPv4(server.trimmingCharacters(in: .whitespaces)) {
                return L(.invalidDNSServer, server)
            }
        }
        return nil
    }
}
