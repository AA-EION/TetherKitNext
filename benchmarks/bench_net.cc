// Benchmarks of the feth / BPF link layer -- **needs root**.
//
// This group fills in the items that had been long left hanging in the last section of docs/BENCHMARKS.md: the real cost of a BPF write,
// the speedup factor of BIOCSBATCHWRITE relative to per-frame writes, the per-frame cost of batch reads, and the
// round-trip latency of the feth data path. Before this, these numbers all came from research estimates (that ~700 ns write cost came from
// `write(/dev/null)`, **not** a measurement on a real BPF descriptor).
//
// How the fixture is set up:
//   feth0 (system side, configured with 10.99.99.1) <-> feth1 (driver side)
//   * driver_perframe / driver_batched: two BPF descriptors both attached to feth1,
//     with BIOCSBATCHWRITE turned off and on respectively, for the A/B of per-frame writes vs batch writes;
//   * system_link: attached to feth0, used to pour traffic toward feth1 to measure read cost.
//
// WARNING: The write cost naturally includes the overhead of "the peer's IP stack receiving the frame and discarding it" -- which is exactly the price the real RX path
// pays for every frame, so measuring this way is correct, and it should not be deliberately bypassed.
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

/// Addresses used by the fixture. 10.99.99/24 almost never conflicts with a real network.
constexpr const char* kSystemIp = "10.99.99.1";
constexpr const char* kProbeIp = "10.99.99.2";

/// Field offsets inside the ARP frame (counted from the start of the frame).
constexpr std::size_t kEtherTypeOffset = 12;
constexpr std::size_t kArpOperationOffset = 20;
constexpr std::size_t kArpSenderIpOffset = 28;
constexpr std::size_t kArpFrameBytes = 42;

std::vector<std::byte> MakeFrame(std::uint32_t length) {
  std::vector<std::byte> frame(length, std::byte{0});
  // Broadcast destination MAC + locally administered source MAC, with EtherType 0x0800. The content itself is unimportant;
  // what matters is that it is a structurally legal Ethernet frame that the peer will actually accept and then discard.
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

/// Builds an ARP request for "who has kSystemIp", used to measure round-trip latency.
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

/// Configures an IPv4 address on an interface (SIOCAIFADDR). Returns an empty string on success.
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

/// The benchmark fixture. Its lifetime is held by a static object in RegisterNetBenchmarks.
struct Fixture {
  std::unique_ptr<net::FethPair> pair;
  std::unique_ptr<net::BpfLink> driver_perframe;  ///< feth1, batch writes turned off
  std::unique_ptr<net::BpfLink> driver_batched;   ///< feth1, batch writes turned on
  std::unique_ptr<net::BpfLink> system_link;      ///< feth0, used to pour in traffic in that direction
  bool batch_write_available = false;
};

Fixture* g_fixture = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// Write benchmark: hands `frames_per_write` frames as one batch to WriteFrames.
///
/// The unit of measurement is the **frame**, so numbers of per-frame writes and batch writes can be compared directly side by side.
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
      break;  // peer queue saturated and similar cases: honestly count by the number of frames actually written.
    }
    written += result->frames_written;
    DoNotOptimize(result->bytes_written);
  }
  return written;
}

/// Read benchmark: pours `burst` frames in from feth0, then reads them back from feth1.
///
/// WARNING: This item measures the combined cost of **write + read**, not pure reading -- the harness can only time the whole
/// closure. To get the pure read cost, subtract the write benchmark value at the same batch size.
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
    // Give up after a bounded number of spins: feth drops frames under queue pressure, so it is normal not to get them all back.
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

/// feth data-path round-trip latency: write an ARP request from the driver side, and wait for the system-side IP stack's reply to be read back.
///
/// This is the only true **latency** metric -- the previous items are all amortized costs in the throughput sense.
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
      break;  // when the neighbor table already has a cache the system side no longer answers; stop here rather than over-report.
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

  // Frames per round: writing is system-call-level overhead (hundreds of nanoseconds to microseconds), and tens of thousands of frames are stable enough,
  // and more would only make the whole round run longer.
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

  // The round-trip latency is only run a few hundred times: each one has to wait for the peer IP stack to really answer, and is an operation under a millisecond yet far slower than
  // the previous items; running too many gives no extra information.
  runner.Add("feth 往返延迟", "ARP 请求 → 系统侧 IP 栈应答", Config{.ops_per_round = 200},
             [](std::uint64_t n) { return BenchArpRoundTrip(n); });

  return true;
}

void ShutdownNetBenchmarks() noexcept {
  if (g_fixture == nullptr) {
    return;
  }
  // The order matters: close the BPF descriptor first, then destroy the feth pair. In the reverse order
  // the interface it is attached to is already gone when BpfLink is destroyed.
  g_fixture->system_link.reset();
  g_fixture->driver_batched.reset();
  g_fixture->driver_perframe.reset();
  g_fixture->pair.reset();
  g_fixture = nullptr;
}

}  // namespace tetherkitnext::bench
