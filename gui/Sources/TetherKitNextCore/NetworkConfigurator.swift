import CTetherKitNext
import Foundation
import TetherKitNextIPC

/// 虚拟网卡的上网方式配置。
///
/// `apply` / `clear` 需要 root（只在 helper 里调）；`query` 不需要。
public enum NetworkConfigurator {
    /// tk_ip_config_t 里每个地址字段的容量，用于定位二维数组 `dns[4][46]` 的行。
    private static let addressStride = Int(TK_ADDRESS_CAPACITY)
    private static let maxDNSServers = Int(TK_DNS_MAX)

    /// 下发配置。
    ///
    /// ⚠️ DHCP 模式下会**阻塞**到拿到租约或超时（库内部上限 10 秒）。这是刻意的：
    /// 「已下发，你自己去轮询」对界面来说没法给出有意义的成败反馈。调用方必须
    /// 保证不在主线程上调它。
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

    /// 撤销网卡上的 IP 配置。
    public static func clear(interface: String) throws {
        var error = tk_error_t()
        let result = interface.withCString { tk_net_clear($0, &error) }
        try check(result, error)
    }

    /// 回读网卡**真实生效**的状态。
    ///
    /// 网卡还不存在时返回全空的状态而不是抛错 —— 界面在会话没起来时也会刷新，
    /// 那时挂一个假的错误只会误导人。
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

// MARK: - 校验

/// 静态 IP 表单的输入校验。
///
/// 放在共享层而不是界面里：helper 侧也要挡一道（XPC 是任何本机进程都能连的），
/// 两边用同一份规则才不会出现「界面通过了但 helper 拒绝」的割裂。
public enum NetworkValidator {
    /// 是否是合法的点分十进制 IPv4 地址。
    public static func isValidIPv4(_ text: String) -> Bool {
        var address = in_addr()
        return text.withCString { inet_pton(AF_INET, $0, &address) } == 1
    }

    /// 是否是合法的子网掩码。
    ///
    /// 比「是合法 IPv4」更严：掩码的二进制必须是连续的 1 后面跟连续的 0。
    /// 255.255.0.255 这种能通过 inet_pton 但作为掩码是错的，而内核对它的反应
    /// 是给出一个诡异的路由，非常难查 —— 所以在入口就挡掉。
    public static func isValidNetmask(_ text: String) -> Bool {
        var address = in_addr()
        guard text.withCString({ inet_pton(AF_INET, $0, &address) }) == 1 else { return false }
        let host = UInt32(bigEndian: address.s_addr)
        // 连续掩码的充要条件：取反加一后是 2 的幂（或为 0，对应 255.255.255.255）。
        let inverted = ~host
        return inverted & (inverted &+ 1) == 0
    }

    /// 逐项校验一份静态 IP 配置，返回第一条不通过的说明；全通过时返回 nil。
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
