// feth / BPF 链路层的基准 —— **需要 root**。
//
// 这一组补的是 docs/BENCHMARKS.md 末节里长期挂着的那几项：BPF 写入的真实成本、
// BIOCSBATCHWRITE 相对逐帧写的提速倍数、批量读取的每帧成本、以及 feth 数据路径的
// 往返延迟。在此之前这些数字全部来自调研推算（那个 ~700 ns 的 write 成本来自
// `write(/dev/null)`，**不是**真实 BPF 描述符上的测量）。
//
// 夹具的搭法：
//   feth0（系统侧，配 10.99.99.1）←→ feth1（驱动侧）
//   * driver_perframe / driver_batched：两个都挂在 feth1 上的 BPF 描述符，
//     分别关掉与打开 BIOCSBATCHWRITE，用来做逐帧写 vs 批量写的 A/B；
//   * system_link：挂在 feth0 上，用来往 feth1 方向灌流量以测读取成本。
//
// ⚠️ 写入成本天然包含「对侧 IP 栈把帧收下并丢弃」的开销 —— 那正是真实 RX 路径
// 每帧都要付的代价，所以这么测才对，不该刻意绕开。
#include "bench_net.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <unistd.h>

#include "harness.h"
#include "tetherkitnext/net/bpf_link.h"
#include "tetherkitnext/net/feth_device.h"

namespace tetherkitnext::bench {
namespace {

constexpr std::uint32_t kFullFrameBytes = 1514;
constexpr std::uint32_t kSmallFrameBytes = 64;

/// 夹具用的地址。10.99.99/24 几乎不会与真实网络冲突。
constexpr const char* kSystemIp = "10.99.99.1";
constexpr const char* kProbeIp = "10.99.99.2";

/// ARP 帧内的字段偏移（帧首起算）。
constexpr std::size_t kEtherTypeOffset = 12;
constexpr std::size_t kArpOperationOffset = 20;
constexpr std::size_t kArpSenderIpOffset = 28;
constexpr std::size_t kArpFrameBytes = 42;

std::vector<std::byte> MakeFrame(std::uint32_t length) {
  std::vector<std::byte> frame(length, std::byte{0});
  // 广播目的 MAC + 本地管理源 MAC，EtherType 填 0x0800。内容本身不重要，
  // 重要的是它是一帧结构合法、对侧会真的收下再丢掉的以太帧。
  for (int i = 0; i < 6; ++i) {
    frame[static_cast<std::size_t>(i)] = std::byte{0xFF};
  }
  frame[6] = std::byte{0x02};
  frame[kEtherTypeOffset] = std::byte{0x08};
  for (std::uint32_t i = 14; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>(i & 0xFFU)};
  }
  return frame;
}

/// 造一个「谁是 kSystemIp」的 ARP 请求，用来测往返延迟。
std::vector<std::byte> MakeArpRequest() {
  std::vector<std::byte> frame(kArpFrameBytes, std::byte{0});
  const auto put = [&frame](std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
    std::size_t index = offset;
    for (const std::uint8_t value : bytes) {
      frame[index++] = std::byte{value};
    }
  };
  put(0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  put(6, {0x02, 0x00, 0x00, 0x99, 0x99, 0x02});
  put(kEtherTypeOffset, {0x08, 0x06});
  put(14, {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01});
  put(22, {0x02, 0x00, 0x00, 0x99, 0x99, 0x02});
  struct in_addr parsed {};
  ::inet_pton(AF_INET, kProbeIp, &parsed);
  std::memcpy(frame.data() + kArpSenderIpOffset, &parsed, sizeof(parsed));
  ::inet_pton(AF_INET, kSystemIp, &parsed);
  std::memcpy(frame.data() + 38, &parsed, sizeof(parsed));
  return frame;
}

[[nodiscard]] bool IsArpReply(const FrameView& view) {
  if (view.length < kArpFrameBytes) {
    return false;
  }
  if (view.data[kEtherTypeOffset] != std::byte{0x08} ||
      view.data[kEtherTypeOffset + 1] != std::byte{0x06}) {
    return false;
  }
  if (view.data[kArpOperationOffset] != std::byte{0x00} ||
      view.data[kArpOperationOffset + 1] != std::byte{0x02}) {
    return false;
  }
  struct in_addr expected {};
  if (::inet_pton(AF_INET, kSystemIp, &expected) != 1) {
    return false;
  }
  return std::memcmp(view.data + kArpSenderIpOffset, &expected, sizeof(expected)) == 0;
}

/// 给接口配 IPv4 地址（SIOCAIFADDR）。成功返回空串。
[[nodiscard]] std::string AssignIpv4(std::string_view interface_name, const char* address) {
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
  if (!fill(request.ifra_addr, address) || !fill(request.ifra_mask, "255.255.255.0")) {
    ::close(fd);
    return "inet_pton 失败";
  }
  const int result = ::ioctl(fd, SIOCAIFADDR, &request);
  const int saved_errno = errno;
  ::close(fd);
  return result == 0 ? std::string{} : Error::FromErrno(saved_errno, "ioctl(SIOCAIFADDR)").ToString();
}

/// 基准夹具。生命周期由 RegisterNetBenchmarks 里的静态对象持有。
struct Fixture {
  std::unique_ptr<net::FethPair> pair;
  std::unique_ptr<net::BpfLink> driver_perframe;  ///< feth1，关掉批量写
  std::unique_ptr<net::BpfLink> driver_batched;   ///< feth1，开启批量写
  std::unique_ptr<net::BpfLink> system_link;      ///< feth0，用于灌入方向流量
  bool batch_write_available = false;
};

Fixture* g_fixture = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// 写入基准：把 `frames_per_write` 帧作为一批交给 WriteFrames。
///
/// 计量单位是**帧**，这样逐帧写与批量写的数字可以直接横向比较。
std::uint64_t BenchWrite(net::BpfLink& link, std::uint64_t frame_iterations,
                         std::uint32_t frame_bytes, std::uint32_t frames_per_write) {
  const std::vector<std::byte> frame = MakeFrame(frame_bytes);
  std::vector<FrameView> batch(frames_per_write);
  for (FrameView& view : batch) {
    view = FrameView{.data = frame.data(), .length = frame_bytes};
  }

  std::uint64_t written = 0;
  while (written < frame_iterations) {
    const auto result = link.WriteFrames(batch);
    if (!result) {
      break;  // 对侧队列打满等情况：诚实地按实际写成的帧数计。
    }
    written += result->frames_written;
    DoNotOptimize(result->bytes_written);
  }
  return written;
}

/// 读取基准：从 feth0 灌 `burst` 帧，再从 feth1 上把它们读回来。
///
/// ⚠️ 这一项测的是**写 + 读**的合计成本，不是纯读取 —— harness 只能给整个
/// 闭包计时。要拿纯读取成本，请减去同批大小下的写入基准值。
std::uint64_t BenchWriteThenRead(std::uint64_t frame_iterations, std::uint32_t frame_bytes,
                                 std::uint32_t burst) {
  Fixture& fixture = *g_fixture;
  const std::vector<std::byte> frame = MakeFrame(frame_bytes);
  std::vector<FrameView> batch(burst);
  for (FrameView& view : batch) {
    view = FrameView{.data = frame.data(), .length = frame_bytes};
  }

  std::uint64_t completed = 0;
  while (completed < frame_iterations) {
    const auto written = fixture.system_link->WriteFrames(batch);
    if (!written || written->frames_written == 0) {
      break;
    }
    std::uint32_t drained = 0;
    // 最多转几圈就放弃：feth 会因队列压力丢帧，等不到全部回来是正常的。
    for (int attempt = 0; attempt < 4 && drained < written->frames_written; ++attempt) {
      const auto received = fixture.driver_batched->ReadFrames();
      if (!received) {
        break;
      }
      drained += static_cast<std::uint32_t>(received->frames.size());
      DoNotOptimize(received->kernel_drops);
    }
    completed += written->frames_written;
  }
  return completed;
}

/// feth 数据路径往返延迟：从驱动侧写 ARP 请求，等系统侧 IP 栈的 reply 读回来。
///
/// 这是唯一一项真正的**延迟**指标 —— 前面几项都是吞吐意义上的摊薄成本。
std::uint64_t BenchArpRoundTrip(std::uint64_t iterations) {
  Fixture& fixture = *g_fixture;
  const std::vector<std::byte> request = MakeArpRequest();
  const std::array<FrameView, 1> batch{
      FrameView{.data = request.data(), .length = kArpFrameBytes}};

  std::uint64_t completed = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (!fixture.driver_perframe->WriteFrames(batch)) {
      break;
    }
    bool answered = false;
    for (int attempt = 0; attempt < 8 && !answered; ++attempt) {
      const auto received = fixture.driver_perframe->ReadFrames();
      if (!received) {
        break;
      }
      for (const FrameView& view : received->frames) {
        if (IsArpReply(view)) {
          answered = true;
          break;
        }
      }
    }
    if (!answered) {
      break;  // 邻居表已经有缓存时系统侧不再应答，就此打住而不是虚报。
    }
    ++completed;
  }
  return completed;
}

}  // namespace

bool RegisterNetBenchmarks(Runner& runner, std::string& skip_reason) {
  if (!net::IsRunningAsRoot()) {
    skip_reason = "需要 root（要建 feth 网卡对并打开 /dev/bpf*）";
    return false;
  }

  static Fixture fixture;

  auto pair = net::FethPair::Create(1500);
  if (!pair) {
    skip_reason = std::format("创建 feth 网卡对失败：{}", pair.error().ToString());
    return false;
  }
  fixture.pair = std::make_unique<net::FethPair>(std::move(*pair));

  const std::string assign_error = AssignIpv4(fixture.pair->SystemSide().Name(), kSystemIp);
  if (!assign_error.empty()) {
    skip_reason = std::format("给系统侧配地址失败：{}", assign_error);
    return false;
  }

  const auto open = [&](std::string_view interface_name,
                        bool batch_write) -> std::unique_ptr<net::BpfLink> {
    net::BpfConfig config;
    config.try_batch_write = batch_write;
    auto link = net::BpfLink::Open(interface_name, config);
    return link ? std::move(*link) : nullptr;
  };

  fixture.driver_perframe = open(fixture.pair->DriverSide().Name(), false);
  fixture.driver_batched = open(fixture.pair->DriverSide().Name(), true);
  fixture.system_link = open(fixture.pair->SystemSide().Name(), true);
  if (fixture.driver_perframe == nullptr || fixture.driver_batched == nullptr ||
      fixture.system_link == nullptr) {
    skip_reason = "打开 BPF 描述符失败";
    return false;
  }
  fixture.batch_write_available = fixture.driver_batched->SupportsBatchWrite();
  g_fixture = &fixture;

  // 每轮的帧数：写入是系统调用级别的开销（百纳秒~微秒量级），几万帧足够稳定，
  // 再多只是让整轮跑得更久。
  constexpr std::uint64_t kWriteOps = 20000;
  constexpr std::uint64_t kReadOps = 20000;

  runner.Add("BPF 写入（逐帧 write）", "1514 字节",
             Config{.ops_per_round = kWriteOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) {
               return BenchWrite(*g_fixture->driver_perframe, n, kFullFrameBytes, 1);
             });
  runner.Add("BPF 写入（逐帧 write）", "64 字节",
             Config{.ops_per_round = kWriteOps, .bytes_per_op = kSmallFrameBytes},
             [](std::uint64_t n) {
               return BenchWrite(*g_fixture->driver_perframe, n, kSmallFrameBytes, 1);
             });

  if (fixture.batch_write_available) {
    for (const std::uint32_t per_write : {8U, 32U, 64U, 128U}) {
      runner.Add("BPF 写入（BIOCSBATCHWRITE）", std::format("1514 字节 × 每次 {} 帧", per_write),
                 Config{.ops_per_round = kWriteOps, .bytes_per_op = kFullFrameBytes},
                 [per_write](std::uint64_t n) {
                   return BenchWrite(*g_fixture->driver_batched, n, kFullFrameBytes, per_write);
                 });
    }
    runner.Add("BPF 写入（BIOCSBATCHWRITE）", "64 字节 × 每次 64 帧",
               Config{.ops_per_round = kWriteOps, .bytes_per_op = kSmallFrameBytes},
               [](std::uint64_t n) {
                 return BenchWrite(*g_fixture->driver_batched, n, kSmallFrameBytes, 64);
               });
  }

  runner.Add("feth 往返（写+读合计）", "1514 字节 × 每批 64 帧",
             Config{.ops_per_round = kReadOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) { return BenchWriteThenRead(n, kFullFrameBytes, 64); });

  // 往返延迟只跑几百次：每次都要等对侧 IP 栈真的应答，是毫秒不到但远慢于
  // 前面几项的操作，跑太多没有额外信息量。
  runner.Add("feth 往返延迟", "ARP 请求 → 系统侧 IP 栈应答", Config{.ops_per_round = 200},
             [](std::uint64_t n) { return BenchArpRoundTrip(n); });

  return true;
}

void ShutdownNetBenchmarks() noexcept {
  if (g_fixture == nullptr) {
    return;
  }
  // 顺序要紧：先关 BPF 描述符，再销毁 feth 网卡对。反过来的话
  // BpfLink 析构时它挂着的接口已经没了。
  g_fixture->system_link.reset();
  g_fixture->driver_batched.reset();
  g_fixture->driver_perframe.reset();
  g_fixture->pair.reset();
  g_fixture = nullptr;
}

}  // namespace tetherkitnext::bench
