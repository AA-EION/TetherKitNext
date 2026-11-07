// Passive discovery of the device's own IPv4 address (peer_address.h).
//
// The rules decide which frames prove "the device owns this address". A false
// positive would suggest a wrong subnet to the user (e.g. taking a remote web
// server's address that a phone forwarded); these cases pin that down.
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <doctest.h>

#include "tetherkitnext/core/peer_address.h"

namespace {

using tetherkitnext::core::OnLinkSenderIpv4;
using tetherkitnext::core::PeerAddressObserver;

constexpr std::uint32_t Ip(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
  return (std::uint32_t{a} << 24) | (std::uint32_t{b} << 16) | (std::uint32_t{c} << 8) | d;
}

void Put16(std::vector<std::byte>& frame, std::size_t offset, std::uint16_t value) {
  frame[offset] = static_cast<std::byte>(value >> 8);
  frame[offset + 1] = static_cast<std::byte>(value & 0xFF);
}

void Put32(std::vector<std::byte>& frame, std::size_t offset, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    frame[offset + i] = static_cast<std::byte>((value >> (24 - 8 * i)) & 0xFF);
  }
}

std::vector<std::byte> ArpFrame(std::uint32_t sender, std::uint32_t target) {
  std::vector<std::byte> frame(14 + 28);
  Put16(frame, 12, 0x0806);
  Put16(frame, 14, 1);       // htype Ethernet
  Put16(frame, 16, 0x0800);  // ptype IPv4
  frame[18] = std::byte{6};
  frame[19] = std::byte{4};
  Put16(frame, 20, 1);  // request
  Put32(frame, 28, sender);
  Put32(frame, 38, target);
  return frame;
}

std::vector<std::byte> Ipv4Frame(std::uint32_t source, std::uint32_t destination) {
  std::vector<std::byte> frame(14 + 20);
  Put16(frame, 12, 0x0800);
  frame[14] = std::byte{0x45};
  Put32(frame, 26, source);
  Put32(frame, 30, destination);
  return frame;
}

}  // namespace

TEST_SUITE("core.peer_address") {

TEST_CASE("ARP sender address is the device's own address") {
  CHECK(OnLinkSenderIpv4(ArpFrame(Ip(192, 168, 42, 2), Ip(192, 168, 42, 1))) ==
        Ip(192, 168, 42, 2));
}

TEST_CASE("ARP probes (sender 0.0.0.0) are ignored") {
  CHECK(OnLinkSenderIpv4(ArpFrame(0, Ip(192, 168, 42, 2))) == 0);
}

TEST_CASE("link-local addresses count: devices without DHCP often use 169.254/16") {
  CHECK(OnLinkSenderIpv4(ArpFrame(Ip(169, 254, 10, 20), Ip(169, 254, 1, 1))) ==
        Ip(169, 254, 10, 20));
}

TEST_CASE("IPv4 to link-scoped destinations proves the source is on the link") {
  // mDNS (224.0.0.251) and limited broadcast (DHCP server replies).
  CHECK(OnLinkSenderIpv4(Ipv4Frame(Ip(10, 0, 0, 5), Ip(224, 0, 0, 251))) == Ip(10, 0, 0, 5));
  CHECK(OnLinkSenderIpv4(Ipv4Frame(Ip(10, 0, 0, 5), Ip(255, 255, 255, 255))) ==
        Ip(10, 0, 0, 5));
}

TEST_CASE("routed unicast and global multicast do not count") {
  // A phone forwards internet traffic: the source is a remote server.
  CHECK(OnLinkSenderIpv4(Ipv4Frame(Ip(142, 250, 1, 1), Ip(172, 19, 0, 2))) == 0);
  // Multicast beyond 224.0.0.0/24 can be routed.
  CHECK(OnLinkSenderIpv4(Ipv4Frame(Ip(142, 250, 1, 1), Ip(239, 255, 255, 250))) == 0);
}

TEST_CASE("implausible sources are rejected") {
  CHECK(OnLinkSenderIpv4(Ipv4Frame(0, Ip(255, 255, 255, 255))) == 0);  // DHCP DISCOVER
  CHECK(OnLinkSenderIpv4(Ipv4Frame(Ip(127, 0, 0, 1), Ip(224, 0, 0, 1))) == 0);
  CHECK(OnLinkSenderIpv4(Ipv4Frame(Ip(224, 0, 0, 5), Ip(224, 0, 0, 1))) == 0);
}

TEST_CASE("truncated and foreign frames are ignored") {
  std::vector<std::byte> arp = ArpFrame(Ip(192, 168, 1, 2), Ip(192, 168, 1, 1));
  arp.resize(30);
  CHECK(OnLinkSenderIpv4(arp) == 0);

  std::vector<std::byte> ipv6(60);
  Put16(ipv6, 12, 0x86DD);
  CHECK(OnLinkSenderIpv4(ipv6) == 0);

  CHECK(OnLinkSenderIpv4({}) == 0);

  std::vector<std::byte> not_v4 = Ipv4Frame(Ip(10, 0, 0, 5), Ip(224, 0, 0, 251));
  not_v4[14] = std::byte{0x65};
  CHECK(OnLinkSenderIpv4(not_v4) == 0);
}

TEST_CASE("observer keeps the latest address and ignores noise") {
  PeerAddressObserver observer;
  CHECK(observer.Latest() == 0);
  observer.Observe(Ipv4Frame(Ip(142, 250, 1, 1), Ip(172, 19, 0, 2)));
  CHECK(observer.Latest() == 0);
  observer.Observe(ArpFrame(Ip(192, 168, 7, 2), Ip(192, 168, 7, 1)));
  CHECK(observer.Latest() == Ip(192, 168, 7, 2));
  observer.Observe(ArpFrame(0, Ip(192, 168, 7, 1)));
  CHECK(observer.Latest() == Ip(192, 168, 7, 2));
  // The device changed address (new DHCP lease, reconfiguration).
  observer.Observe(Ipv4Frame(Ip(192, 168, 8, 2), Ip(224, 0, 0, 251)));
  CHECK(observer.Latest() == Ip(192, 168, 8, 2));
}

}  // TEST_SUITE("core.peer_address")
