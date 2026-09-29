// Microbenchmarks of the infrastructure layer.
//
// These numbers are the baseline for later judging "where the real bottleneck is". In particular:
//   * memcpy of one frame vs one SPSC enqueue+dequeue -- used to verify the claim in the FrameRing comments that
//     "the copy is not the bottleneck";
//   * Cross-thread throughput of the lock-free queue -- determines the theoretical ceiling of the data path.
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

/// The two most common frame lengths on Ethernet: an MTU-sized full frame and a small TCP ACK frame.
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
// memcpy baseline
// ---------------------------------------------------------------------------

/// Plain memcpy: the lower bound of FrameRing's single-copy cost.
///
/// Note these are numbers under a **fully hot cache**: both source and destination stay resident in L1. The source on the real RX path is a buffer written by USB
/// DMA, which is cold to the CPU cache, so the actual cost will be higher. What is measured here is the lower bound,
/// used to subtract from FrameRing's round-trip cost to estimate the queue's own overhead.
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
// Byte-order reads and writes
// ---------------------------------------------------------------------------

/// The 11 LoadLe32 calls needed to parse an RNDIS_PACKET_MSG header.
std::uint64_t BenchParsePacketHeader(std::uint64_t iterations) {
  // A 44-byte REMOTE_NDIS_PACKET_MSG header, deliberately placed at an odd offset,
  // to also verify the cost of unaligned reads (should be zero).
  std::vector<std::byte> buffer(64);
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = std::byte{static_cast<unsigned char>(i)};
  }

  std::uint32_t accumulator = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    const std::byte* header = buffer.data() + 1;  // unaligned
    for (std::size_t field = 0; field < 11; ++field) {
      accumulator += LoadLe32(header + field * 4);
    }
    DoNotOptimize(accumulator);
  }
  return iterations;
}

// ---------------------------------------------------------------------------
// SPSC queue
// ---------------------------------------------------------------------------

/// One single-thread enqueue+dequeue round: measures pure index arithmetic and memory-ordering overhead (no cross-core cache round trip).
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

/// Cross-thread movement: this is the situation the data path really encounters, including a cache coherence round trip.
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

/// One single-thread "reserve-write-commit-read-release" round, including one memcpy.
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

/// Cross-thread frame movement: the real model of the RX path (libusb callback thread -> BPF write thread).
///
/// `batch_size` = 1 is equivalent to per-frame publishing (one release store per frame, and the cache line holding the write position
/// bounces across cores once per frame); > 1 uses BatchWrite / BatchRead to amortize the publishing.
/// This comparison is direct evidence for the single highest-payoff optimization in the entire data-path design.
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
      // batch destructor -> one PublishWrite(n)
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
      // Simulate that the consumer really reads the frame content (BPF write reads the whole frame).
      DoNotOptimize(view.data[0]);
      ++received;
    }
    // batch destructor -> one PublishRead(n)
  }
  producer.join();
  return iterations;
}

// ---------------------------------------------------------------------------
// Statistics counters
// ---------------------------------------------------------------------------

/// The counter update executed for every frame: it must be cheap enough to ignore.
///
/// ClobberMemory() must be put in the loop body: consecutive relaxed atomic read-modify-writes are **allowed** to be
/// merged by the compiler (N increments of +1 folded into one +N), and without the barrier the measurement would be 0 ns/op -- that is not the counter's
/// true per-call cost. On the real data path a system call sits between every two counts, so there is no opportunity
/// to merge, and adding a compiler barrier (no runtime instruction) is the correct model.
std::uint64_t BenchCounterUpdate(std::uint64_t iterations) {
  DirectionCounters counters;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    counters.AddFrame(kFullFrameBytes);
    ClobberMemory();
  }
  DoNotOptimize(counters.frames);
  return iterations;
}

/// Comparison: how much more expensive it would be to use fetch_add (a true atomic read-modify-write, which on arm64 is LSE's ldadd).
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
  // Comparison of the effect of batch publishing. This is the most important group of numbers for the whole data path: the same queue,
  // the same amount moved, changing only "how many frames each release store publishes".
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
