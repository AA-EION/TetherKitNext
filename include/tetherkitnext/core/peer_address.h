// Passive discovery of the attached device's own IPv4 address.
//
// Why: not every RNDIS device is a phone that hands out a DHCP lease. Drones,
// remote controllers, FPV goggles, single-board computers in USB gadget mode
// and cameras are *local links*: they often have a fixed address and no DHCP
// server, so the host never learns where the device is. The bridge sees every
// frame the device sends, so it can learn that address without sending
// anything.
//
// Only frames that can *only* originate from the device itself are trusted.
// A phone forwards internet traffic, so the IPv4 source of an ordinary
// unicast packet is usually some remote server, not the phone. These are
// trustworthy:
//   * ARP: the sender protocol address is the sender's own interface address
//     (ARP is never forwarded across a router). Probes (sender 0.0.0.0) are
//     ignored.
//   * IPv4 to the limited broadcast 255.255.255.255 or to link-local
//     multicast 224.0.0.0/24 (mDNS, IGMP, DHCP server replies): routers never
//     forward these, so the source is on the link.
//
// Hot path: called for every device→host frame from the RX injection thread.
// No allocation, no locks, a handful of byte loads, and one relaxed atomic
// store only when the address changes.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "tetherkitnext/common/byte_order.h"

namespace tetherkitnext::core {

inline constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr std::uint16_t kEtherTypeArp = 0x0806;

/// Whether `address` (host order) can be a unicast interface address.
[[nodiscard]] constexpr bool IsPlausibleHostIpv4(std::uint32_t address) noexcept {
  const std::uint32_t first_octet = address >> 24;
  return address != 0 && address != 0xFFFFFFFFU  // unspecified, broadcast
         && first_octet != 0 && first_octet != 127  // "this network", loopback
         && first_octet < 224;                      // multicast and reserved
}

/// The IPv4 address (host byte order) this frame proves the sender owns, or 0.
[[nodiscard]] inline std::uint32_t OnLinkSenderIpv4(std::span<const std::byte> frame) noexcept {
  constexpr std::size_t kEthernetHeader = 14;
  if (frame.size() < kEthernetHeader) {
    return 0;
  }
  const std::byte* base = frame.data();
  const std::uint16_t ether_type = LoadBe16(base + 12);

  if (ether_type == kEtherTypeArp) {
    // htype(2) ptype(2) hlen(1) plen(1) oper(2) sha(6) spa(4) tha(6) tpa(4) = 28
    constexpr std::size_t kArpBody = 28;
    if (frame.size() < kEthernetHeader + kArpBody) {
      return 0;
    }
    const std::byte* arp = base + kEthernetHeader;
    if (LoadBe16(arp) != 1 || LoadBe16(arp + 2) != kEtherTypeIpv4 ||
        arp[4] != std::byte{6} || arp[5] != std::byte{4}) {
      return 0;
    }
    const std::uint32_t sender = LoadBe<std::uint32_t>(arp + 14);
    return IsPlausibleHostIpv4(sender) ? sender : 0;
  }

  if (ether_type == kEtherTypeIpv4) {
    constexpr std::size_t kIpv4MinHeader = 20;
    if (frame.size() < kEthernetHeader + kIpv4MinHeader) {
      return 0;
    }
    const std::byte* ip = base + kEthernetHeader;
    if ((std::to_integer<std::uint8_t>(ip[0]) >> 4) != 4) {
      return 0;
    }
    const std::uint32_t source = LoadBe<std::uint32_t>(ip + 12);
    const std::uint32_t destination = LoadBe<std::uint32_t>(ip + 16);
    const bool link_scoped_destination =
        destination == 0xFFFFFFFFU || (destination & 0xFFFFFF00U) == 0xE0000000U;
    return link_scoped_destination && IsPlausibleHostIpv4(source) ? source : 0;
  }

  return 0;
}

/// Remembers the most recent address `OnLinkSenderIpv4` found.
///
/// One writer (the RX injection thread), any number of readers (status
/// snapshots). The value is a hint for the UI, so relaxed ordering suffices.
class PeerAddressObserver {
 public:
  void Observe(std::span<const std::byte> frame) noexcept {
    const std::uint32_t address = OnLinkSenderIpv4(frame);
    if (address != 0 && address != latest_.load(std::memory_order_relaxed)) {
      latest_.store(address, std::memory_order_relaxed);
    }
  }

  /// Host byte order; 0 when nothing has been seen yet.
  [[nodiscard]] std::uint32_t Latest() const noexcept {
    return latest_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<std::uint32_t> latest_{0};
};

}  // namespace tetherkitnext::core
