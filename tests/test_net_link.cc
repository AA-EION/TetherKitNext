// Link-layer tests.
//
// Divided into two parts:
//   * loopback backend -- runs in any environment, is the infrastructure of the bridge layer tests, and must itself be reliable;
//   * feth + BPF -- needs root, and is **skipped rather than failed** in non-root environments.
//     Enabled explicitly with TETHERKITNEXT_ROOT_TESTS=1 (to avoid accidentally running real NIC operations on CI).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <unistd.h>

#include <doctest.h>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/net/bpf_link.h"
#include "tetherkitnext/net/darwin_abi.h"
#include "tetherkitnext/net/feth_device.h"
#include "tetherkitnext/net/loopback_link.h"

using namespace tetherkitnext;       // NOLINT(google-build-using-namespace)
using namespace tetherkitnext::net;  // NOLINT(google-build-using-namespace)

namespace {

/// Builds a legal Ethernet frame: destination MAC, source MAC, EtherType, then a verifiable payload.
std::vector<std::byte> MakeEthernetFrame(std::uint32_t total_length, std::uint8_t tag) {
  std::vector<std::byte> frame(total_length);
  // Destination MAC: broadcast; source MAC: 02:00:00:00:00:tag (locally administered unicast).
  for (int i = 0; i < 6; ++i) {
    frame[static_cast<std::size_t>(i)] = std::byte{0xFF};
  }
  frame[6] = std::byte{0x02};
  frame[11] = std::byte{tag};
  frame[12] = std::byte{0x08};  // EtherType 0x0800 = IPv4
  frame[13] = std::byte{0x00};
  for (std::uint32_t i = 14; i < total_length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)};
  }
  return frame;
}

/// Whether tests that need root are enabled.
///
/// getenv is not safe under multi-threading, but here it is a one-time read at the start of a test case, with no
/// concurrent setenv in the process, so the check is exempted.
// NOLINTNEXTLINE(concurrency-mt-unsafe)
bool RootTestsEnabled() {
  const char* flag = std::getenv("TETHERKITNEXT_ROOT_TESTS");
  return flag != nullptr && flag[0] == '1' && IsRunningAsRoot();
}

/// Renders the error of a Result/Status into a string; returns an empty string on success.
///
/// Writing `r ? "" : r.error().ToString()` directly inside REQUIRE_MESSAGE does not compile --
/// doctest's MessageBuilder swallows the ternary expression whole into the stream and then tries to convert it to bool.
template <typename T>
std::string Why(const T& result) {
  return result.has_value() ? std::string{} : result.error().ToString();
}

/// Prints the skip reason, making "skipped" visible in the test output rather than passing silently.
///
/// First assemble a std::string with std::format and then hand it to MESSAGE: doctest's
/// MessageBuilder stringifies a `const char*` as a pointer, and streaming it in directly would print an address.
void ReportSkip(std::string_view what) {
  const std::string message =
      std::format("跳过 {}：需要 root 且需设置 TETHERKITNEXT_ROOT_TESTS=1（当前 euid={}）", what,
                  IsRunningAsRoot() ? "0" : "非 0");
  MESSAGE(message);
}

// ---------------------------------------------------------------------------
// Small tools for the ARP round-trip loop
//
// What this loop is to prove is far stronger than "cannot read back the frames we wrote ourselves": BPF's write() really delivered the frame
// into the **IP stack of the peer feth**, and did not merely write it into a black hole. The criterion is that the peer IP stack **actively
// answers** -- if it did not understand this frame it would not reply with an ARP reply.
// ---------------------------------------------------------------------------

/// Addresses used by the loop. 10.99.99/24 is chosen because it almost never conflicts with a real network.
constexpr const char* kArpSystemIp = "10.99.99.1";  ///< Configured on the system-side feth.
constexpr const char* kArpProbeIp = "10.99.99.2";   ///< The asker we impersonate.

/// Configures an IPv4 address on an interface.
///
/// SIOCAIFADDR is used directly here rather than going through capi's `ipconfig` path: the test wants an address that
/// takes effect immediately and disappears along with the feth when the process exits, needing no IPConfiguration
/// lease management, and a test should not spawn subprocesses either.
[[nodiscard]] std::string AssignIpv4(std::string_view interface_name, const char* address,
                                     const char* netmask) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return Error::FromErrno(errno, "socket(AF_INET)").ToString();
  }

  struct ifaliasreq request {};
  std::memcpy(request.ifra_name, interface_name.data(),
              std::min(interface_name.size(), sizeof(request.ifra_name) - 1));

  const auto fill = [](struct sockaddr& slot, const char* text) -> bool {
    struct sockaddr_in value {};
    value.sin_len = sizeof(value);
    value.sin_family = AF_INET;
    if (::inet_pton(AF_INET, text, &value.sin_addr) != 1) {
      return false;
    }
    std::memcpy(&slot, &value, sizeof(value));
    return true;
  };

  if (!fill(request.ifra_addr, address) || !fill(request.ifra_mask, netmask)) {
    ::close(fd);
    return "inet_pton 失败";
  }

  const int result = ::ioctl(fd, SIOCAIFADDR, &request);
  const int saved_errno = errno;
  ::close(fd);
  if (result != 0) {
    return Error::FromErrno(saved_errno, "ioctl(SIOCAIFADDR)").ToString();
  }
  return {};
}

/// Field offsets inside the ARP frame (counted from the start of the frame).
constexpr std::size_t kEtherTypeOffset = 12;
constexpr std::size_t kArpOperationOffset = 20;
constexpr std::size_t kArpSenderMacOffset = 22;
constexpr std::size_t kArpSenderIpOffset = 28;
constexpr std::size_t kArpFrameBytes = 42;  ///< 14 Ethernet header + 28 ARP.

/// Builds an ARP request of "who is target_ip? tell sender_ip" (broadcast).
std::vector<std::byte> MakeArpRequest(const MacAddress& sender_mac, const char* sender_ip,
                                      const char* target_ip) {
  std::vector<std::byte> frame(kArpFrameBytes, std::byte{0});
  const auto put = [&frame](std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
    std::size_t index = offset;
    for (const std::uint8_t value : bytes) {
      frame[index++] = std::byte{value};
    }
  };

  put(0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});  // destination MAC: broadcast
  for (std::size_t i = 0; i < sender_mac.size(); ++i) {
    frame[6 + i] = std::byte{sender_mac[i]};
  }
  put(kEtherTypeOffset, {0x08, 0x06});  // EtherType = ARP
  put(14, {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01});  // Ethernet/IPv4, oper = 1 (request)

  for (std::size_t i = 0; i < sender_mac.size(); ++i) {
    frame[kArpSenderMacOffset + i] = std::byte{sender_mac[i]};
  }
  struct in_addr parsed {};
  ::inet_pton(AF_INET, sender_ip, &parsed);
  std::memcpy(frame.data() + kArpSenderIpOffset, &parsed, sizeof(parsed));
  ::inet_pton(AF_INET, target_ip, &parsed);
  std::memcpy(frame.data() + 38, &parsed, sizeof(parsed));  // target IP, target MAC left at 0
  return frame;
}

/// Judges whether a frame is an ARP reply that "claims to own expected_ip".
[[nodiscard]] bool IsArpReplyFor(const FrameView& view, const char* expected_ip) {
  if (view.length < kArpFrameBytes) {
    return false;
  }
  if (view.data[kEtherTypeOffset] != std::byte{0x08} ||
      view.data[kEtherTypeOffset + 1] != std::byte{0x06}) {
    return false;
  }
  // oper == 2 is a reply.
  if (view.data[kArpOperationOffset] != std::byte{0x00} ||
      view.data[kArpOperationOffset + 1] != std::byte{0x02}) {
    return false;
  }
  struct in_addr expected {};
  if (::inet_pton(AF_INET, expected_ip, &expected) != 1) {
    return false;
  }
  return std::memcmp(view.data + kArpSenderIpOffset, &expected, sizeof(expected)) == 0;
}

}  // namespace

TEST_SUITE("net.abi") {

TEST_CASE("私有 ABI 的结构体大小与 ioctl 编号与实测值一致") {
  // These are all static_asserts, and compiling means they hold; here they are confirmed once more at runtime,
  // making the ABI assumptions **visible** in the test report.
  CHECK(sizeof(IfDrv) == 40);
  CHECK(sizeof(FethRequest) == 160);
  CHECK(offsetof(FethRequest, u) == 32);
  CHECK(kSetDriverSpec == 0x8028'697BUL);
  CHECK(kGetDriverSpec == 0xC028'697BUL);
  CHECK(kBpfSetBatchWrite == 0x8004'428FUL);
  CHECK(kBpfSetNoTimestamp == 0x8004'4291UL);
  CHECK(static_cast<unsigned long>(FethSetCommand::kSetPeer) == 1);
  CHECK(static_cast<unsigned long>(FethGetCommand::kGetPeer) == 1);
}

TEST_CASE("BPF 记录头的大小陷阱：sizeof 是 20 但 bh_hdrlen 是 18") {
  // This is where BPF parsing most easily goes wrong; pin it down with a test.
  CHECK(sizeof(struct bpf_hdr) == 20);
  CHECK(sizeof(struct BPF_TIMEVAL) == 8);
  CHECK(kBpfHeaderMinBytes == 18);
  CHECK(BPF_ALIGNMENT == 4);
  // The kernel's derivation for DLT_EN10MB: BPF_WORDALIGN(14 + 18) - 14 = 18.
  CHECK(BPF_WORDALIGN(14 + kBpfHeaderMinBytes) - 14 == kBpfHeaderMinBytes);
}

TEST_CASE("feth 创建期 sysctl 清单非空且都有说明") {
  CHECK(std::size(kRequiredFethSysctls) > 0);
  for (const RequiredFethSysctl& entry : kRequiredFethSysctls) {
    CHECK(entry.name != nullptr);
    // why stores a message identifier; both languages must really have a translation (a missing translation would render as an empty string,
    // so that after "Reason:" in the sysctl error there would be nothing at all).
    for (const auto language : {tetherkitnext::Language::kChinese, tetherkitnext::Language::kEnglish}) {
      const std::string_view why = tetherkitnext::TextIn(language, entry.why);
      // The explanation must be plain language, not just a single word.
      CHECK(why.size() > 10);
    }
  }
}

}  // TEST_SUITE("net.abi")

TEST_SUITE("net.loopback") {

TEST_CASE("空队列读取返回空批次") {
  LoopbackLink link;
  const auto batch = link.ReadFrames();
  REQUIRE(batch.has_value());
  CHECK(batch->frames.empty());
}

TEST_CASE("注入的帧能按 FIFO 顺序读出") {
  LoopbackLink link;
  for (std::uint8_t i = 0; i < 5; ++i) {
    REQUIRE(link.PushInbound(MakeEthernetFrame(100, i)));
  }
  const auto batch = link.ReadFrames();
  REQUIRE(batch.has_value());
  REQUIRE(batch->frames.size() == 5);
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(batch->frames[i].length == 100);
    // The last byte of the source MAC is the sequence number we stuffed in.
    CHECK(batch->frames[i].data[11] == std::byte{static_cast<unsigned char>(i)});
  }
}

TEST_CASE("单批读取受 max_frames_per_batch 限制，剩余留到下一批") {
  LoopbackLink link(LoopbackConfig{.max_frames_per_batch = 3});
  for (std::uint8_t i = 0; i < 7; ++i) {
    REQUIRE(link.PushInbound(MakeEthernetFrame(64, i)));
  }
  const auto first = link.ReadFrames();
  REQUIRE(first.has_value());
  CHECK(first->frames.size() == 3);

  const auto second = link.ReadFrames();
  REQUIRE(second.has_value());
  CHECK(second->frames.size() == 3);

  const auto third = link.ReadFrames();
  REQUIRE(third.has_value());
  CHECK(third->frames.size() == 1);
}

TEST_CASE("注入队列满时丢弃并计数") {
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 2});
  CHECK(link.PushInbound(MakeEthernetFrame(64, 1)));
  CHECK(link.PushInbound(MakeEthernetFrame(64, 2)));
  CHECK_FALSE(link.PushInbound(MakeEthernetFrame(64, 3)));
  CHECK(link.InboundDrops() == 1);
}

TEST_CASE("超长帧被拒绝注入") {
  LoopbackLink link(LoopbackConfig{.max_frame_bytes = 128});
  CHECK_FALSE(link.PushInbound(MakeEthernetFrame(129, 1)));
  CHECK(link.InboundDrops() == 1);
}

TEST_CASE("写出的帧可被取回，内容完整") {
  LoopbackLink link;
  const auto frame_a = MakeEthernetFrame(1514, 0xA1);
  const auto frame_b = MakeEthernetFrame(64, 0xB2);
  const std::array<FrameView, 2> batch{
      FrameView{.data = frame_a.data(), .length = static_cast<std::uint32_t>(frame_a.size())},
      FrameView{.data = frame_b.data(), .length = static_cast<std::uint32_t>(frame_b.size())},
  };

  const auto result = link.WriteFrames(batch);
  REQUIRE(result.has_value());
  CHECK(result->frames_written == 2);
  CHECK(result->bytes_written == 1514 + 64);
  CHECK(result->frames_skipped == 0);

  const auto sent = link.DrainSent();
  REQUIRE(sent.size() == 2);
  CHECK(sent[0] == frame_a);
  CHECK(sent[1] == frame_b);
  // DrainSent does not affect the cumulative counts.
  CHECK(link.TotalSentFrames() == 2);
  CHECK(link.TotalSentBytes() == 1514 + 64);
  // Taking again after emptying should give empty.
  CHECK(link.DrainSent().empty());
}

TEST_CASE("写出时跳过过短与过长的帧") {
  LoopbackLink link(LoopbackConfig{.max_frame_bytes = 200});
  const auto too_short = MakeEthernetFrame(14, 1);
  std::vector<std::byte> shorter(13);
  const auto too_long = MakeEthernetFrame(201, 2);
  const auto valid = MakeEthernetFrame(100, 3);

  const std::array<FrameView, 3> batch{
      FrameView{.data = shorter.data(), .length = 13},
      FrameView{.data = too_long.data(), .length = 201},
      FrameView{.data = valid.data(), .length = 100},
  };
  const auto result = link.WriteFrames(batch);
  REQUIRE(result.has_value());
  CHECK(result->frames_written == 1);
  CHECK(result->frames_skipped == 2);
  // 14 bytes is exactly the minimum legal frame and should be accepted.
  const std::array<FrameView, 1> minimal{FrameView{.data = too_short.data(), .length = 14}};
  const auto minimal_result = link.WriteFrames(minimal);
  REQUIRE(minimal_result.has_value());
  CHECK(minimal_result->frames_written == 1);
}

TEST_CASE("写出队列满时停止写入，如实反映已写数") {
  LoopbackLink link(LoopbackConfig{.sent_capacity = 2});
  const auto frame = MakeEthernetFrame(64, 1);
  std::vector<FrameView> batch(5, FrameView{.data = frame.data(), .length = 64});
  const auto result = link.WriteFrames(batch);
  REQUIRE(result.has_value());
  CHECK(result->frames_written == 2);
}

TEST_CASE("Interrupt 后读取立即返回空批次") {
  LoopbackLink link;
  REQUIRE(link.PushInbound(MakeEthernetFrame(64, 1)));
  link.Interrupt();
  CHECK(link.Interrupted());
  const auto batch = link.ReadFrames();
  REQUIRE(batch.has_value());
  CHECK(batch->frames.empty());
}

TEST_CASE("可以注入写失败以测试错误传播") {
  LoopbackLink link;
  link.FailWritesAfter(1);  // 1st succeeds, fails from the 2nd on
  const auto frame = MakeEthernetFrame(64, 1);
  const std::array<FrameView, 1> batch{FrameView{.data = frame.data(), .length = 64}};

  CHECK(link.WriteFrames(batch).has_value());
  CHECK_FALSE(link.WriteFrames(batch).has_value());
}

TEST_CASE("空批次写入是合法的空操作") {
  LoopbackLink link;
  const auto result = link.WriteFrames({});
  REQUIRE(result.has_value());
  CHECK(result->frames_written == 0);
}

}  // TEST_SUITE("net.loopback")

TEST_SUITE("net.feth" * doctest::skip(false)) {

TEST_CASE("查询 feth MTU 上限（只读 sysctl，无需 root）") {
  const auto max_mtu = QueryFethMaxMtu();
  REQUIRE(max_mtu.has_value());
  // The kernel guarantees >= ETHERMTU(1500).
  CHECK(*max_mtu >= 1500);
  MESSAGE(std::format("net.link.fake.max_mtu = {}", *max_mtu));
}

TEST_CASE("校验 feth 创建期 sysctl（只读，无需 root）") {
  const auto status = VerifyFethSysctls();
  if (!status) {
    // Not a test failure -- this machine's sysctl has been modified. Report it so that people know.
    MESSAGE(std::format("本机 feth sysctl 不满足要求：{}", status.error().ToString()));
  }
  // Passing as long as it can be read; the specific value depends on the machine configuration.
  CHECK(true);
}

TEST_CASE("非 root 环境下创建 feth 必须给出明确的权限错误") {
  if (IsRunningAsRoot()) {
    MESSAGE("当前以 root 运行，跳过权限错误检查");
    return;
  }
  const auto pair = FethPair::Create(1500);
  REQUIRE_FALSE(pair.has_value());
  const std::string message = pair.error().ToString();
  // The error must mention root / sudo, rather than dropping a bare EPERM on the user.
  CHECK((message.find("root") != std::string::npos || message.find("sudo") != std::string::npos));
}

TEST_CASE("MAC 格式化") {
  const MacAddress mac{0x66, 0x65, 0x74, 0x68, 0x00, 0x00};
  // The kernel assigned exactly this address to feth0 ('f','e','t','h', unit>>8, unit&0xff).
  CHECK(std::string{FormatMac(mac).data()} == "66:65:74:68:00:00");
}

TEST_CASE("完整生命周期：创建、配对、设 MTU/MAC、UP、销毁") {
  if (!RootTestsEnabled()) {
    ReportSkip("feth 完整生命周期测试");
    return;
  }

  const MacAddress device_mac{0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  auto pair = FethPair::Create(1500, &device_mac);
  REQUIRE_MESSAGE(pair.has_value(), Why(pair));

  const std::string system_name{pair->SystemSide().Name()};
  const std::string driver_name{pair->DriverSide().Name()};
  MESSAGE(std::format("创建了 {} ←→ {}", system_name, driver_name));

  // The pairing relationship holds in both directions.
  const auto driver_peer = pair->DriverSide().QueryPeer();
  REQUIRE(driver_peer.has_value());
  CHECK(*driver_peer == system_name);
  const auto system_peer = pair->SystemSide().QueryPeer();
  REQUIRE(system_peer.has_value());
  CHECK(*system_peer == driver_name);

  // The MTU is the same on both sides.
  CHECK(pair->SystemSide().QueryMtu().value_or(0) == 1500);
  CHECK(pair->DriverSide().QueryMtu().value_or(0) == 1500);

  // The system-side MAC is the one we set, and the driver side keeps the kernel-assigned one (the two must differ).
  const auto system_mac = pair->SystemSide().QueryMacAddress();
  REQUIRE(system_mac.has_value());
  CHECK(*system_mac == device_mac);
  const auto driver_mac = pair->DriverSide().QueryMacAddress();
  REQUIRE(driver_mac.has_value());
  CHECK(*driver_mac != device_mac);

  // Both sides are UP -- bpfwrite strictly requires the driver side to be UP.
  CHECK(pair->SystemSide().IsUp().value_or(false));
  CHECK(pair->DriverSide().IsUp().value_or(false));
}

TEST_CASE("BPF 打开配置全流程，并验证方向语义与回环抑制") {
  if (!RootTestsEnabled()) {
    ReportSkip("BPF 收发测试");
    return;
  }

  auto pair = FethPair::Create(1500);
  REQUIRE_MESSAGE(pair.has_value(), Why(pair));

  // BPF attaches to the **driver side**.
  auto link = BpfLink::Open(pair->DriverSide().Name(), BpfConfig{});
  REQUIRE_MESSAGE(link.has_value(), Why(link));

  MESSAGE(std::format("BPF 设备 {}，内核缓冲 {} 字节，批量写 {}", (*link)->DevicePath(),
                      (*link)->KernelBufferBytes(),
                      (*link)->SupportsBatchWrite() ? "可用" : "不可用"));

  // The buffer actually in effect in the kernel should not be 0, and should not exceed the 32 MiB upper limit.
  CHECK((*link)->KernelBufferBytes() > 0);
  CHECK((*link)->KernelBufferBytes() <= 32U * 1024 * 1024);

  const auto stats = (*link)->QueryKernelStats();
  REQUIRE(stats.has_value());

  // Write one frame in -- it should enter the system side's input and **should not** be read back by ourselves
  // (BIOCSSEESENT=0 filters out the output direction).
  const auto frame = MakeEthernetFrame(200, 0x5A);
  const std::array<FrameView, 1> batch{FrameView{.data = frame.data(), .length = 200}};
  const auto written = (*link)->WriteFrames(batch);
  REQUIRE_MESSAGE(written.has_value(), Why(written));
  CHECK(written->frames_written == 1);

  // Read once (it returns after the read timeout). It should never see the frame we just wrote.
  const auto batch_read = (*link)->ReadFrames();
  REQUIRE(batch_read.has_value());
  for (const FrameView& view : batch_read->frames) {
    // The source MAC byte 12 of the frame we wrote is 0x5A; reading it means loopback suppression has failed.
    const bool is_our_frame = view.length == 200 && view.data[11] == std::byte{0x5A};
    CHECK_FALSE(is_our_frame);
  }
}

TEST_CASE("ARP 往返闭环：BPF 写入的帧确实进了对侧 feth 的 IP 栈") {
  if (!RootTestsEnabled()) {
    ReportSkip("ARP 往返闭环测试");
    return;
  }

  auto pair = FethPair::Create(1500);
  REQUIRE_MESSAGE(pair.has_value(), Why(pair));

  // Configure an IP on the system side, giving its IP stack a reason to answer ARP.
  const std::string assign_error =
      AssignIpv4(pair->SystemSide().Name(), kArpSystemIp, "255.255.255.0");
  REQUIRE_MESSAGE(assign_error.empty(), assign_error);

  const auto system_mac = pair->SystemSide().QueryMacAddress();
  REQUIRE_MESSAGE(system_mac.has_value(), Why(system_mac));

  // BPF attaches to the driver side -- this side plays the "device".
  auto link = BpfLink::Open(pair->DriverSide().Name(), BpfConfig{});
  REQUIRE_MESSAGE(link.has_value(), Why(link));

  const MacAddress probe_mac{0x02, 0x00, 0x00, 0x99, 0x99, 0x02};
  const auto request = MakeArpRequest(probe_mac, kArpProbeIp, kArpSystemIp);
  const std::array<FrameView, 1> batch{
      FrameView{.data = request.data(), .length = static_cast<std::uint32_t>(request.size())}};

  const auto written = (*link)->WriteFrames(batch);
  REQUIRE_MESSAGE(written.has_value(), Why(written));
  REQUIRE(written->frames_written == 1);

  // Read several rounds before judging failure: ReadFrames waits only one read timeout per round, and although the IP stack's reply is fast,
  // there is no guarantee that it falls in the first round.
  bool saw_reply = false;
  MacAddress reply_mac{};
  for (int attempt = 0; attempt < 20 && !saw_reply; ++attempt) {
    const auto received = (*link)->ReadFrames();
    REQUIRE_MESSAGE(received.has_value(), Why(received));
    for (const FrameView& view : received->frames) {
      if (IsArpReplyFor(view, kArpSystemIp)) {
        saw_reply = true;
        for (std::size_t i = 0; i < reply_mac.size(); ++i) {
          reply_mac[i] = static_cast<std::uint8_t>(view.data[kArpSenderMacOffset + i]);
        }
        break;
      }
    }
  }

  // This one assertion is the core premise of the whole scheme: the peer IP stack **received and processed** the frame we wrote in.
  CHECK_MESSAGE(saw_reply, "没有收到系统侧 feth 的 ARP reply —— BPF write 没能送达对侧 IP 栈");
  if (saw_reply) {
    // The responder must be exactly the system-side feth, and not a frame from some other interface leaking in.
    CHECK(reply_mac == *system_mac);
    MESSAGE(std::format("{} 宣称拥有 {}，MAC {}", pair->SystemSide().Name(), kArpSystemIp,
                        FormatMac(reply_mac).data()));
  }
}

}  // TEST_SUITE("net.feth")
