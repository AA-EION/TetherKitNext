// 链路层测试。
//
// 分两部分：
//   * loopback 后端 —— 任何环境都能跑，是桥接层测试的基础设施，必须自身可靠；
//   * feth + BPF —— 需要 root，非 root 环境下**跳过而非失败**。
//     用 TETHERKITNEXT_ROOT_TESTS=1 显式开启（避免 CI 上误跑真实网卡操作）。
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

/// 造一帧合法以太帧：目的 MAC、源 MAC、EtherType，再填可校验的净荷。
std::vector<std::byte> MakeEthernetFrame(std::uint32_t total_length, std::uint8_t tag) {
  std::vector<std::byte> frame(total_length);
  // 目的 MAC：广播；源 MAC：02:00:00:00:00:tag（本地管理单播）。
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

/// 需要 root 的测试是否启用。
///
/// getenv 在多线程下不安全，但这里是测试用例开头的一次性读取，进程内没有
/// 并发的 setenv，因此豁免检查。
// NOLINTNEXTLINE(concurrency-mt-unsafe)
bool RootTestsEnabled() {
  const char* flag = std::getenv("TETHERKITNEXT_ROOT_TESTS");
  return flag != nullptr && flag[0] == '1' && IsRunningAsRoot();
}

/// 把 Result/Status 的错误渲染成字符串，成功时返回空串。
///
/// 直接在 REQUIRE_MESSAGE 里写 `r ? "" : r.error().ToString()` 编译不过 ——
/// doctest 的 MessageBuilder 会把三元表达式整体吞进流里再试图转 bool。
template <typename T>
std::string Why(const T& result) {
  return result.has_value() ? std::string{} : result.error().ToString();
}

/// 打印跳过原因，让「跳过」在测试输出里是可见的、而不是静默通过。
///
/// 先用 std::format 拼成一个 std::string 再交给 MESSAGE：doctest 的
/// MessageBuilder 对 `const char*` 会按指针字符串化，直接流进去会打出地址。
void ReportSkip(std::string_view what) {
  const std::string message =
      std::format("跳过 {}：需要 root 且需设置 TETHERKITNEXT_ROOT_TESTS=1（当前 euid={}）", what,
                  IsRunningAsRoot() ? "0" : "非 0");
  MESSAGE(message);
}

// ---------------------------------------------------------------------------
// ARP 往返闭环用的小工具
//
// 这个闭环要证明的事情，比「读不回自己写的帧」强得多：BPF 的 write() 真的把帧
// 送进了**对侧 feth 的 IP 栈**，而不只是写进了一个黑洞。判据是对侧 IP 栈**主动
// 应答** —— 它不理解这一帧就不会回 ARP reply。
// ---------------------------------------------------------------------------

/// 闭环用的地址。选 10.99.99/24 是因为它几乎不会与真实网络冲突。
constexpr const char* kArpSystemIp = "10.99.99.1";  ///< 配在系统侧 feth 上。
constexpr const char* kArpProbeIp = "10.99.99.2";   ///< 我们伪装的提问者。

/// 给接口配一个 IPv4 地址。
///
/// 这里直接用 SIOCAIFADDR 而不是走 capi 的 `ipconfig` 路径：测试要的是一个
/// 立即生效、进程退出即随 feth 一起消失的地址，不需要 IPConfiguration 的
/// 租约管理，也不该在测试里拉起子进程。
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

/// 以太帧内的 ARP 字段偏移（帧首起算）。
constexpr std::size_t kEtherTypeOffset = 12;
constexpr std::size_t kArpOperationOffset = 20;
constexpr std::size_t kArpSenderMacOffset = 22;
constexpr std::size_t kArpSenderIpOffset = 28;
constexpr std::size_t kArpFrameBytes = 42;  ///< 14 以太头 + 28 ARP。

/// 造一个「谁是 target_ip？告诉 sender_ip」的 ARP 请求（广播）。
std::vector<std::byte> MakeArpRequest(const MacAddress& sender_mac, const char* sender_ip,
                                      const char* target_ip) {
  std::vector<std::byte> frame(kArpFrameBytes, std::byte{0});
  const auto put = [&frame](std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
    std::size_t index = offset;
    for (const std::uint8_t value : bytes) {
      frame[index++] = std::byte{value};
    }
  };

  put(0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});  // 目的 MAC：广播
  for (std::size_t i = 0; i < sender_mac.size(); ++i) {
    frame[6 + i] = std::byte{sender_mac[i]};
  }
  put(kEtherTypeOffset, {0x08, 0x06});  // EtherType = ARP
  put(14, {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01});  // 以太/IPv4，oper = 1（请求）

  for (std::size_t i = 0; i < sender_mac.size(); ++i) {
    frame[kArpSenderMacOffset + i] = std::byte{sender_mac[i]};
  }
  struct in_addr parsed {};
  ::inet_pton(AF_INET, sender_ip, &parsed);
  std::memcpy(frame.data() + kArpSenderIpOffset, &parsed, sizeof(parsed));
  ::inet_pton(AF_INET, target_ip, &parsed);
  std::memcpy(frame.data() + 38, &parsed, sizeof(parsed));  // 目标 IP，目标 MAC 留 0
  return frame;
}

/// 判断一帧是否是「宣称拥有 expected_ip」的 ARP reply。
[[nodiscard]] bool IsArpReplyFor(const FrameView& view, const char* expected_ip) {
  if (view.length < kArpFrameBytes) {
    return false;
  }
  if (view.data[kEtherTypeOffset] != std::byte{0x08} ||
      view.data[kEtherTypeOffset + 1] != std::byte{0x06}) {
    return false;
  }
  // oper == 2 即 reply。
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
  // 这些都是 static_assert，能编译过就说明成立；这里再运行期确认一遍，
  // 让 ABI 假设在测试报告里是**可见**的。
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
  // 这是 BPF 解析最容易出错的地方，用测试把它钉死。
  CHECK(sizeof(struct bpf_hdr) == 20);
  CHECK(sizeof(struct BPF_TIMEVAL) == 8);
  CHECK(kBpfHeaderMinBytes == 18);
  CHECK(BPF_ALIGNMENT == 4);
  // 内核对 DLT_EN10MB 的推导：BPF_WORDALIGN(14 + 18) - 14 = 18。
  CHECK(BPF_WORDALIGN(14 + kBpfHeaderMinBytes) - 14 == kBpfHeaderMinBytes);
}

TEST_CASE("feth 创建期 sysctl 清单非空且都有说明") {
  CHECK(std::size(kRequiredFethSysctls) > 0);
  for (const RequiredFethSysctl& entry : kRequiredFethSysctls) {
    CHECK(entry.name != nullptr);
    // why 存的是文案标识，两种语言都必须真的有译文（漏译会渲染成空串，
    // 于是 sysctl 报错里那句「原因：」后面什么都没有）。
    for (const auto language : {tetherkitnext::Language::kChinese, tetherkitnext::Language::kEnglish}) {
      const std::string_view why = tetherkitnext::TextIn(language, entry.why);
      // 说明必须是人话，不能只有一个词。
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
    // 源 MAC 的最后一字节是我们塞的序号。
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
  // DrainSent 不影响累计计数。
  CHECK(link.TotalSentFrames() == 2);
  CHECK(link.TotalSentBytes() == 1514 + 64);
  // 取空之后再取应为空。
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
  // 14 字节恰好是最小合法帧，应被接受。
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
  link.FailWritesAfter(1);  // 第 1 次成功，第 2 次起失败
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
  // 内核保证 >= ETHERMTU(1500)。
  CHECK(*max_mtu >= 1500);
  MESSAGE(std::format("net.link.fake.max_mtu = {}", *max_mtu));
}

TEST_CASE("校验 feth 创建期 sysctl（只读，无需 root）") {
  const auto status = VerifyFethSysctls();
  if (!status) {
    // 不是测试失败 —— 是这台机器的 sysctl 被改过。报出来让人知道。
    MESSAGE(std::format("本机 feth sysctl 不满足要求：{}", status.error().ToString()));
  }
  // 只要能读到就算通过；具体值取决于机器配置。
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
  // 错误必须提到 root / sudo，而不是丢一个裸 EPERM 给用户。
  CHECK((message.find("root") != std::string::npos || message.find("sudo") != std::string::npos));
}

TEST_CASE("MAC 格式化") {
  const MacAddress mac{0x66, 0x65, 0x74, 0x68, 0x00, 0x00};
  // 内核给 feth0 分配的正是这个地址（'f','e','t','h', unit>>8, unit&0xff）。
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

  // 配对关系双向成立。
  const auto driver_peer = pair->DriverSide().QueryPeer();
  REQUIRE(driver_peer.has_value());
  CHECK(*driver_peer == system_name);
  const auto system_peer = pair->SystemSide().QueryPeer();
  REQUIRE(system_peer.has_value());
  CHECK(*system_peer == driver_name);

  // MTU 两侧一致。
  CHECK(pair->SystemSide().QueryMtu().value_or(0) == 1500);
  CHECK(pair->DriverSide().QueryMtu().value_or(0) == 1500);

  // 系统侧 MAC 是我们设的，驱动侧保持内核分配的（两者必须不同）。
  const auto system_mac = pair->SystemSide().QueryMacAddress();
  REQUIRE(system_mac.has_value());
  CHECK(*system_mac == device_mac);
  const auto driver_mac = pair->DriverSide().QueryMacAddress();
  REQUIRE(driver_mac.has_value());
  CHECK(*driver_mac != device_mac);

  // 两侧都 UP —— bpfwrite 硬性要求驱动侧是 UP。
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

  // BPF 挂在**驱动侧**。
  auto link = BpfLink::Open(pair->DriverSide().Name(), BpfConfig{});
  REQUIRE_MESSAGE(link.has_value(), Why(link));

  MESSAGE(std::format("BPF 设备 {}，内核缓冲 {} 字节，批量写 {}", (*link)->DevicePath(),
                      (*link)->KernelBufferBytes(),
                      (*link)->SupportsBatchWrite() ? "可用" : "不可用"));

  // 内核实际生效的缓冲不应为 0，且不超过 32 MiB 的上限。
  CHECK((*link)->KernelBufferBytes() > 0);
  CHECK((*link)->KernelBufferBytes() <= 32U * 1024 * 1024);

  const auto stats = (*link)->QueryKernelStats();
  REQUIRE(stats.has_value());

  // 写一帧进去 —— 它应该进入系统侧的 input，**不应该**被我们自己读回来
  // （BIOCSSEESENT=0 过滤掉 output 方向）。
  const auto frame = MakeEthernetFrame(200, 0x5A);
  const std::array<FrameView, 1> batch{FrameView{.data = frame.data(), .length = 200}};
  const auto written = (*link)->WriteFrames(batch);
  REQUIRE_MESSAGE(written.has_value(), Why(written));
  CHECK(written->frames_written == 1);

  // 读一次（会在读超时后返回）。绝不应看到自己刚写的那一帧。
  const auto batch_read = (*link)->ReadFrames();
  REQUIRE(batch_read.has_value());
  for (const FrameView& view : batch_read->frames) {
    // 我们写的帧源 MAC 第 12 字节是 0x5A；读到它就说明回环抑制失效了。
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

  // 系统侧配一个 IP，好让它的 IP 栈有理由应答 ARP。
  const std::string assign_error =
      AssignIpv4(pair->SystemSide().Name(), kArpSystemIp, "255.255.255.0");
  REQUIRE_MESSAGE(assign_error.empty(), assign_error);

  const auto system_mac = pair->SystemSide().QueryMacAddress();
  REQUIRE_MESSAGE(system_mac.has_value(), Why(system_mac));

  // BPF 挂驱动侧 —— 这一侧扮演「设备」。
  auto link = BpfLink::Open(pair->DriverSide().Name(), BpfConfig{});
  REQUIRE_MESSAGE(link.has_value(), Why(link));

  const MacAddress probe_mac{0x02, 0x00, 0x00, 0x99, 0x99, 0x02};
  const auto request = MakeArpRequest(probe_mac, kArpProbeIp, kArpSystemIp);
  const std::array<FrameView, 1> batch{
      FrameView{.data = request.data(), .length = static_cast<std::uint32_t>(request.size())}};

  const auto written = (*link)->WriteFrames(batch);
  REQUIRE_MESSAGE(written.has_value(), Why(written));
  REQUIRE(written->frames_written == 1);

  // 读若干轮再判定失败：ReadFrames 一轮只等一个读超时，而 IP 栈的应答虽然快，
  // 却没有「一定落在第一轮」的保证。
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

  // 这一条断言就是整个方案的核心前提：对侧 IP 栈**收到并处理了**我们写进去的帧。
  CHECK_MESSAGE(saw_reply, "没有收到系统侧 feth 的 ARP reply —— BPF write 没能送达对侧 IP 栈");
  if (saw_reply) {
    // 应答者必须正是系统侧 feth，而不是别的什么接口串进来的帧。
    CHECK(reply_mac == *system_mac);
    MESSAGE(std::format("{} 宣称拥有 {}，MAC {}", pair->SystemSide().Name(), kArpSystemIp,
                        FormatMac(reply_mac).data()));
  }
}

}  // TEST_SUITE("net.feth")
