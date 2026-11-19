import Darwin
import Foundation

/// One network interface as `getifaddrs(3)` reports it, at one instant.
///
/// Read without privileges: every user can enumerate interfaces and read
/// their byte counters, which is what the Advanced → Interfaces page shows.
public struct InterfaceSnapshot: Sendable, Equatable, Identifiable {
    public var name: String
    /// `IFF_UP`: administratively enabled.
    public var isUp: Bool
    /// `IFF_RUNNING`: resources allocated / link present.
    public var isRunning: Bool
    public var isLoopback: Bool
    /// Hardware address (`aa:bb:cc:dd:ee:ff`), when the link layer has one.
    public var linkAddress: String?
    public var mtu: UInt32
    /// Cumulative byte counters (`struct if_data64` via `NET_RT_IFLIST2`).
    ///
    /// 64-bit on purpose: the `struct if_data` that `getifaddrs` hands out
    /// has 32-bit counters that wrap every 4 GiB, and a wrap is then
    /// indistinguishable from an interface being recreated with fresh
    /// counters. With 64 bits a decrease can only mean a reset.
    public var receivedBytes: UInt64
    public var sentBytes: UInt64
    public var ipv4Addresses: [String]
    public var ipv6Addresses: [String]

    public var id: String { name }

    /// Up, running, and carrying at least one address: the interfaces worth
    /// showing by default.
    public var isActive: Bool {
        isUp && isRunning && !(ipv4Addresses.isEmpty && ipv6Addresses.isEmpty)
    }

    public init(name: String, isUp: Bool, isRunning: Bool, isLoopback: Bool,
                linkAddress: String?, mtu: UInt32, receivedBytes: UInt64, sentBytes: UInt64,
                ipv4Addresses: [String], ipv6Addresses: [String]) {
        self.name = name
        self.isUp = isUp
        self.isRunning = isRunning
        self.isLoopback = isLoopback
        self.linkAddress = linkAddress
        self.mtu = mtu
        self.receivedBytes = receivedBytes
        self.sentBytes = sentBytes
        self.ipv4Addresses = ipv4Addresses
        self.ipv6Addresses = ipv6Addresses
    }
}

/// Live receive / transmit rate of one interface.
public struct InterfaceRates: Sendable, Equatable {
    public var receiveBitsPerSecond: Double
    public var transmitBitsPerSecond: Double

    public static let zero = InterfaceRates(receiveBitsPerSecond: 0, transmitBitsPerSecond: 0)

    public init(receiveBitsPerSecond: Double, transmitBitsPerSecond: Double) {
        self.receiveBitsPerSecond = receiveBitsPerSecond
        self.transmitBitsPerSecond = transmitBitsPerSecond
    }
}

/// Turns successive counter snapshots into rates.
///
/// Pure value logic so the edge cases are unit-tested:
///   * The first sample of an interface has nothing to compare with: 0.
///   * A counter that went down means the interface was destroyed and
///     recreated under the same name (feth pairs between sessions do exactly
///     this): that direction reports 0 for one round instead of a bogus spike.
///   * Interfaces absent from a round are forgotten, so a later reappearance
///     starts fresh.
public struct InterfaceRateTracker: Sendable {
    private struct Sample: Sendable {
        var received: UInt64
        var sent: UInt64
        var time: Double
    }

    private var previous: [String: Sample] = [:]

    public init() {}

    /// Feeds one round of snapshots taken at `time` (seconds, monotonic) and
    /// returns the rate of every interface in it.
    public mutating func update(with snapshots: [InterfaceSnapshot],
                                at time: Double) -> [String: InterfaceRates] {
        var rates: [String: InterfaceRates] = [:]
        var next: [String: Sample] = [:]
        for snapshot in snapshots {
            let sample = Sample(received: snapshot.receivedBytes, sent: snapshot.sentBytes,
                                time: time)
            next[snapshot.name] = sample
            guard let old = previous[snapshot.name], time > old.time else {
                rates[snapshot.name] = .zero
                continue
            }
            let elapsed = time - old.time
            rates[snapshot.name] = InterfaceRates(
                receiveBitsPerSecond: Self.rate(sample.received, old.received, elapsed),
                transmitBitsPerSecond: Self.rate(sample.sent, old.sent, elapsed))
        }
        previous = next
        return rates
    }

    static func rate(_ current: UInt64, _ old: UInt64, _ elapsed: Double) -> Double {
        guard current >= old else { return 0 }
        return Double(current - old) * 8 / elapsed
    }
}

/// Reads the interface list with `getifaddrs(3)`.
public enum InterfaceSampler {
    /// All interfaces, in the order the kernel lists them.
    public static func snapshot() -> [InterfaceSnapshot] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }

        var order: [String] = []
        var byName: [String: InterfaceSnapshot] = [:]

        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let item = entry.pointee
            guard let address = item.ifa_addr else { continue }
            let name = String(cString: item.ifa_name)
            let flags = Int32(bitPattern: item.ifa_flags)

            if byName[name] == nil {
                order.append(name)
                byName[name] = InterfaceSnapshot(
                    name: name,
                    isUp: flags & IFF_UP != 0,
                    isRunning: flags & IFF_RUNNING != 0,
                    isLoopback: flags & IFF_LOOPBACK != 0,
                    linkAddress: nil, mtu: 0, receivedBytes: 0, sentBytes: 0,
                    ipv4Addresses: [], ipv6Addresses: [])
            }

            switch Int32(address.pointee.sa_family) {
            case AF_LINK:
                // For AF_LINK entries ifa_data points at the interface's
                // struct if_data; only the MTU is taken from it (its byte
                // counters are 32-bit, see `receivedBytes`).
                if let data = item.ifa_data?.assumingMemoryBound(to: if_data.self) {
                    byName[name]?.mtu = data.pointee.ifi_mtu
                }
                byName[name]?.linkAddress = linkAddress(address)
            case AF_INET:
                if let text = numericHost(address) { byName[name]?.ipv4Addresses.append(text) }
            case AF_INET6:
                if let text = numericHost(address) { byName[name]?.ipv6Addresses.append(text) }
            default:
                break
            }
        }
        let counters = counters64()
        return order.compactMap { name in
            guard var snapshot = byName[name] else { return nil }
            if let pair = counters[name] {
                snapshot.receivedBytes = pair.received
                snapshot.sentBytes = pair.sent
            }
            return snapshot
        }
    }

    /// 64-bit byte counters per interface name, from the routing socket's
    /// `NET_RT_IFLIST2` dump (`RTM_IFINFO2` records carry `struct if_data64`).
    /// The same source `netstat -b` uses; no privileges required.
    static func counters64() -> [String: (received: UInt64, sent: UInt64)] {
        var mib: [Int32] = [CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0]
        var length = 0
        var buffer: [UInt8] = []
        // Interfaces can appear between sizing and reading (ENOMEM): leave headroom, and
        // retry a few times instead of losing the whole sample.
        var read = false
        for _ in 0..<3 {
            guard sysctl(&mib, UInt32(mib.count), nil, &length, nil, 0) == 0, length > 0 else {
                return [:]
            }
            length += length / 8
            buffer = [UInt8](repeating: 0, count: length)
            if sysctl(&mib, UInt32(mib.count), &buffer, &length, nil, 0) == 0 {
                read = true
                break
            }
            guard errno == ENOMEM else { return [:] }
        }
        guard read else { return [:] }

        var result: [String: (received: UInt64, sent: UInt64)] = [:]
        buffer.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            var offset = 0
            let headerSize = MemoryLayout<if_msghdr>.size
            while offset + headerSize <= length {
                let message = base.advanced(by: offset)
                // ifm_msglen leads every routing message; stop on garbage.
                let messageLength = Int(message.loadUnaligned(as: UInt16.self))
                guard messageLength > 0, offset + messageLength <= length else { break }
                let type = Int32(message.loadUnaligned(fromByteOffset: 3, as: UInt8.self))
                if type == RTM_IFINFO2,
                   messageLength >= MemoryLayout<if_msghdr2>.size {
                    let header = message.loadUnaligned(as: if_msghdr2.self)
                    var nameBuffer = [CChar](repeating: 0, count: Int(IF_NAMESIZE) + 1)
                    if if_indextoname(UInt32(header.ifm_index), &nameBuffer) != nil {
                        let name = String(
                            decoding: nameBuffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) },
                            as: UTF8.self)
                        result[name] = (header.ifm_data.ifi_ibytes, header.ifm_data.ifi_obytes)
                    }
                }
                offset += messageLength
            }
        }
        return result
    }

    private static func numericHost(_ address: UnsafeMutablePointer<sockaddr>) -> String? {
        var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        let status: Int32
        if Int32(address.pointee.sa_family) == AF_INET6 {
            // The kernel embeds a link-local address's interface index in bytes 2-3 of the
            // address (the KAME convention), so getifaddrs returns e.g. fe80:4::1. Move it
            // to sin6_scope_id, as ifconfig does, so it prints as fe80::1%en0.
            var copy = address.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { $0.pointee }
            var embedded: UInt32 = 0
            withUnsafeMutableBytes(of: &copy.sin6_addr) { bytes in
                guard bytes[0] == 0xFE, bytes[1] & 0xC0 == 0x80 else { return }
                embedded = UInt32(bytes[2]) << 8 | UInt32(bytes[3])
                bytes[2] = 0
                bytes[3] = 0
            }
            if embedded != 0 && copy.sin6_scope_id == 0 {
                copy.sin6_scope_id = embedded
            }
            status = withUnsafePointer(to: &copy) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    getnameinfo($0, socklen_t(MemoryLayout<sockaddr_in6>.size),
                                &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
                }
            }
        } else {
            status = getnameinfo(address, socklen_t(address.pointee.sa_len),
                                 &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
        }
        guard status == 0 else { return nil }
        return String(decoding: host.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) },
                      as: UTF8.self)
    }

    /// `sockaddr_dl` → "aa:bb:cc:dd:ee:ff". The address follows the interface
    /// name inside `sdl_data` (LLADDR() in <net/if_dl.h>).
    private static func linkAddress(_ address: UnsafeMutablePointer<sockaddr>) -> String? {
        address.withMemoryRebound(to: sockaddr_dl.self, capacity: 1) { link -> String? in
            let length = Int(link.pointee.sdl_alen)
            guard length == 6 else { return nil }
            let dataOffset = MemoryLayout<sockaddr_dl>.offset(of: \sockaddr_dl.sdl_data) ?? 8
            let start = UnsafeRawPointer(link)
                .advanced(by: dataOffset + Int(link.pointee.sdl_nlen))
            let bytes = (0..<length).map { start.load(fromByteOffset: $0, as: UInt8.self) }
            guard bytes.contains(where: { $0 != 0 }) else { return nil }
            return bytes.map { String(format: "%02x", $0) }.joined(separator: ":")
        }
    }
}
