// 基础设施层的微基准。
//
// 这些数字是后续判断「哪里是真瓶颈」的基线。特别是：
//   * memcpy 一帧 vs 一次 SPSC 入队出队 —— 用来验证 FrameRing 注释里那个
//     「拷贝不是瓶颈」的论断；
//   * 无锁队列的跨线程吞吐 —— 决定数据路径的理论上限。
#include "bench_common.h"

#include <atomic>
#include <cstddef>
#include <format>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include "harness.h"
#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/common/frame_ring.h"
#include "tetherkitnext/common/spsc_ring.h"
#include "tetherkitnext/common/stats.h"

namespace tetherkitnext::bench {
namespace {

/// 以太网上最常见的两种帧长：MTU 满帧与 TCP ACK 小帧。
constexpr std::uint32_t kFullFrameBytes = 1514;
constexpr std::uint32_t kSmallFrameBytes = 64;

std::vector<std::byte> MakeFrame(std::uint32_t length) {
  std::vector<std::byte> frame(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>(i & 0xFFU)};
  }
  return frame;
}

// ---------------------------------------------------------------------------
// memcpy 基线
// ---------------------------------------------------------------------------

/// 纯 memcpy：FrameRing 单次拷贝成本的下界。
///
/// 注意这是**全热缓存**下的数字：源与目标都常驻 L1。真实 RX 路径的源是 USB
/// DMA 写入的缓冲，对 CPU 缓存是冷的，实际成本会更高。这里量的是下界，
/// 用来和 FrameRing 的往返成本相减，估算队列本身的开销。
std::uint64_t BenchMemcpy(std::uint64_t iterations, std::uint32_t frame_bytes) {
  const std::vector<std::byte> source = MakeFrame(frame_bytes);
  std::vector<std::byte> destination(frame_bytes);
  for (std::uint64_t i = 0; i < iterations; ++i) {
    std::memcpy(destination.data(), source.data(), frame_bytes);
    DoNotOptimize(destination.data());
  }
  return iterations;
}

// ---------------------------------------------------------------------------
// 字节序读写
// ---------------------------------------------------------------------------

/// 解析一个 RNDIS_PACKET_MSG 头部所需的 11 次 LoadLe32。
std::uint64_t BenchParsePacketHeader(std::uint64_t iterations) {
  // 44 字节的 REMOTE_NDIS_PACKET_MSG 头部，刻意放在奇数偏移上，
  // 顺便验证未对齐读取的代价（应该为零）。
  std::vector<std::byte> buffer(64);
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = std::byte{static_cast<unsigned char>(i)};
  }

  std::uint32_t accumulator = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    const std::byte* header = buffer.data() + 1;  // 未对齐
    for (std::size_t field = 0; field < 11; ++field) {
      accumulator += LoadLe32(header + field * 4);
    }
    DoNotOptimize(accumulator);
  }
  return iterations;
}

// ---------------------------------------------------------------------------
// SPSC 队列
// ---------------------------------------------------------------------------

/// 单线程入队+出队一轮：测纯索引运算与内存序开销（无跨核缓存往返）。
std::uint64_t BenchSpscRoundTripSameThread(std::uint64_t iterations) {
  SpscRing<std::uint64_t> ring(1024);
  std::uint64_t sink = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (!ring.TryPush(i)) {
      return i;
    }
    if (!ring.TryPop(sink)) {
      return i;
    }
    DoNotOptimize(sink);
  }
  return iterations;
}

/// 跨线程搬运：这是数据路径真正会遇到的情形，包含缓存一致性往返。
std::uint64_t BenchSpscCrossThread(std::uint64_t iterations) {
  SpscRing<std::uint64_t> ring(4096);
  std::atomic<bool> start{false};

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::uint64_t sent = 0;
    while (sent < iterations) {
      if (ring.TryPush(sent)) {
        ++sent;
      }
    }
  });

  start.store(true, std::memory_order_release);
  std::uint64_t received = 0;
  std::uint64_t value = 0;
  while (received < iterations) {
    if (ring.TryPop(value)) {
      ++received;
      DoNotOptimize(value);
    }
  }
  producer.join();
  return iterations;
}

// ---------------------------------------------------------------------------
// FrameRing
// ---------------------------------------------------------------------------

/// 单线程「预留-写入-提交-读取-释放」一轮，含一次 memcpy。
std::uint64_t BenchFrameRingRoundTrip(std::uint64_t iterations, std::uint32_t frame_bytes) {
  FrameRing ring(1024, kMaxEthernetFrameBytes);
  const std::vector<std::byte> source = MakeFrame(frame_bytes);

  for (std::uint64_t i = 0; i < iterations; ++i) {
    const std::span<std::byte> dst = ring.BeginWrite();
    if (dst.empty()) {
      return i;
    }
    std::memcpy(dst.data(), source.data(), frame_bytes);
    ring.CommitWrite(frame_bytes);

    const FrameView view = ring.BeginRead();
    if (view.Empty()) {
      return i;
    }
    DoNotOptimize(view.data);
    ring.CommitRead();
  }
  return iterations;
}

/// 跨线程帧搬运：RX 路径（libusb 回调线程 → BPF 写线程）的真实模型。
///
/// `batch_size` = 1 时等价于逐帧发布（每帧一次 release store，写位置所在缓存行
/// 每帧跨核弹一次）；> 1 时用 BatchWrite / BatchRead 把发布摊薄。
/// 这一组对比是整个数据路径设计里收益最高的单点优化的直接证据。
std::uint64_t BenchFrameRingCrossThread(std::uint64_t iterations, std::uint32_t frame_bytes,
                                       std::uint32_t batch_size) {
  FrameRing ring(4096, kMaxEthernetFrameBytes);
  std::atomic<bool> start{false};

  std::thread producer([&] {
    const std::vector<std::byte> source = MakeFrame(frame_bytes);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::uint64_t sent = 0;
    while (sent < iterations) {
      auto batch = ring.BeginBatchWrite();
      for (std::uint32_t i = 0; i < batch_size && sent < iterations; ++i) {
        const std::span<std::byte> dst = batch.Begin();
        if (dst.empty()) {
          break;
        }
        std::memcpy(dst.data(), source.data(), frame_bytes);
        batch.Commit(frame_bytes);
        ++sent;
      }
      // batch 析构 → 一次 PublishWrite(n)
    }
  });

  start.store(true, std::memory_order_release);
  std::uint64_t received = 0;
  while (received < iterations) {
    auto batch = ring.BeginBatchRead();
    for (std::uint32_t i = 0; i < batch_size && received < iterations; ++i) {
      const FrameView view = batch.Next();
      if (view.Empty()) {
        break;
      }
      // 模拟消费者会真的读到帧内容（BPF write 会读整帧）。
      DoNotOptimize(view.data[0]);
      ++received;
    }
    // batch 析构 → 一次 PublishRead(n)
  }
  producer.join();
  return iterations;
}

// ---------------------------------------------------------------------------
// 统计计数器
// ---------------------------------------------------------------------------

/// 每帧都要执行的计数器更新：必须便宜到可以忽略。
///
/// 循环体里必须放 ClobberMemory()：relaxed 原子的连续读-改-写是**允许**被编译器
/// 合并的（N 次 +1 折叠成一次 +N），不加屏障测出来会是 0 ns/op —— 那不是计数器
/// 真实的单次成本。真实数据路径上每两次计数之间都夹着一次系统调用，不存在合并
/// 机会，所以加编译屏障（无运行时指令）才是正确的模型。
std::uint64_t BenchCounterUpdate(std::uint64_t iterations) {
  DirectionCounters counters;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    counters.AddFrame(kFullFrameBytes);
    ClobberMemory();
  }
  DoNotOptimize(counters.frames);
  return iterations;
}

/// 对比：如果用 fetch_add（真原子读-改-写，arm64 上是 LSE 的 ldadd）会贵多少。
std::uint64_t BenchCounterUpdateFetchAdd(std::uint64_t iterations) {
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint64_t> bytes{0};
  for (std::uint64_t i = 0; i < iterations; ++i) {
    frames.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(kFullFrameBytes, std::memory_order_relaxed);
    ClobberMemory();
  }
  DoNotOptimize(frames);
  return iterations;
}

}  // namespace

void RegisterCommonBenchmarks(Runner& runner) {
  constexpr std::uint64_t kMicroOps = 2'000'000;
  constexpr std::uint64_t kFrameOps = 500'000;

  runner.Add("memcpy 基线", "1514 字节帧",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) { return BenchMemcpy(n, kFullFrameBytes); });
  runner.Add("memcpy 基线", "64 字节帧",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kSmallFrameBytes},
             [](std::uint64_t n) { return BenchMemcpy(n, kSmallFrameBytes); });

  runner.Add("字节序", "解析 44 字节 RNDIS 包头（未对齐）", Config{.ops_per_round = kMicroOps},
             [](std::uint64_t n) { return BenchParsePacketHeader(n); });

  runner.Add("SPSC 队列", "单线程 push+pop", Config{.ops_per_round = kMicroOps},
             [](std::uint64_t n) { return BenchSpscRoundTripSameThread(n); });
  runner.Add("SPSC 队列", "跨线程搬运", Config{.measure_rounds = 5, .ops_per_round = kFrameOps},
             [](std::uint64_t n) { return BenchSpscCrossThread(n); });

  runner.Add("FrameRing", "单线程往返（1514 字节）",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) { return BenchFrameRingRoundTrip(n, kFullFrameBytes); });
  runner.Add("FrameRing", "单线程往返（64 字节）",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kSmallFrameBytes},
             [](std::uint64_t n) { return BenchFrameRingRoundTrip(n, kSmallFrameBytes); });
  // 批量发布的效果对比。这是整个数据路径最重要的一组数字：同一条队列、
  // 同样的搬运量，只改「每次 release store 发布多少帧」。
  for (const std::uint32_t batch : {1U, 4U, 8U, 32U, 64U}) {
    runner.Add(
        "FrameRing 批量发布",
        std::format("跨线程 1514 字节 × batch={}", batch),
        Config{.measure_rounds = 5, .ops_per_round = kFrameOps, .bytes_per_op = kFullFrameBytes},
        [batch](std::uint64_t n) { return BenchFrameRingCrossThread(n, kFullFrameBytes, batch); });
  }
  runner.Add(
      "FrameRing 批量发布", "跨线程 64 字节 × batch=1",
      Config{.measure_rounds = 5, .ops_per_round = kFrameOps, .bytes_per_op = kSmallFrameBytes},
      [](std::uint64_t n) { return BenchFrameRingCrossThread(n, kSmallFrameBytes, 1); });
  runner.Add(
      "FrameRing 批量发布", "跨线程 64 字节 × batch=32",
      Config{.measure_rounds = 5, .ops_per_round = kFrameOps, .bytes_per_op = kSmallFrameBytes},
      [](std::uint64_t n) { return BenchFrameRingCrossThread(n, kSmallFrameBytes, 32); });

  runner.Add("统计计数器", "relaxed load+store（本项目做法）", Config{.ops_per_round = kMicroOps},
             [](std::uint64_t n) { return BenchCounterUpdate(n); });
  runner.Add("统计计数器", "fetch_add 对比", Config{.ops_per_round = kMicroOps},
             [](std::uint64_t n) { return BenchCounterUpdateFetchAdd(n); });
}

}  // namespace tetherkitnext::bench
