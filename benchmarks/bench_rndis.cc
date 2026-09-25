// RNDIS 编解码的微基准。
//
// 这两条路径每帧都要走一次，是除系统调用之外唯一按帧计费的 CPU 成本：
//   TX：PacketMessageWriter 把以太帧包成 RNDIS_PACKET_MSG（含多包聚合与对齐）
//   RX：PacketMessageReader 从一个 bulk IN 传输里逐帧拆出来
#include "bench_rndis.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

#include "harness.h"
#include "tetherkitnext/rndis/packet_codec.h"
#include "tetherkitnext/rndis/protocol.h"

namespace tetherkitnext::bench {
namespace {

constexpr std::uint32_t kFullFrameBytes = 1514;
constexpr std::uint32_t kSmallFrameBytes = 64;

std::vector<std::byte> MakeFrame(std::uint32_t length) {
  std::vector<std::byte> frame(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>(i & 0xFFU)};
  }
  return frame;
}

/// TX 编码：把 `frames_per_transfer` 帧聚合进一个传输。
///
/// 度量单位是**帧**而不是传输，这样不同聚合度的数字可以直接横向比较。
std::uint64_t BenchEncode(std::uint64_t frame_iterations, std::uint32_t frame_bytes,
                          std::uint32_t frames_per_transfer, std::uint32_t alignment_bytes) {
  // 传输缓冲要装得下 frames_per_transfer 个满帧。
  const std::size_t capacity =
      static_cast<std::size_t>(frames_per_transfer) *
          (rndis::kPacketMsgHeaderBytes + frame_bytes + alignment_bytes) +
      64;
  std::vector<std::byte> transfer(capacity);
  const std::vector<std::byte> frame = MakeFrame(frame_bytes);

  const rndis::PacketMessageWriter::Limits limits{
      .max_transfer_bytes = static_cast<std::uint32_t>(capacity),
      .max_messages = frames_per_transfer,
      .alignment_bytes = alignment_bytes,
  };

  std::uint64_t encoded = 0;
  while (encoded < frame_iterations) {
    rndis::PacketMessageWriter writer(transfer, limits, /*endpoint_max_packet=*/512);
    for (std::uint32_t i = 0; i < frames_per_transfer && encoded < frame_iterations; ++i) {
      if (!writer.TryAppend(frame)) {
        break;
      }
      ++encoded;
    }
    const std::uint32_t produced = writer.Finish();
    DoNotOptimize(produced);
    // ★ 必须同时强制观测**目标缓冲的内容**。只 DoNotOptimize(Finish() 的返回值)
    // 时，编译器发现这块缓冲此后再也没人读，就会把 TryAppend 里那次 memcpy 整个
    // 消除掉 —— 于是测出来 14.5 ns/帧，比裸 memcpy 1514 字节（25 ns）还快，
    // 一眼就知道不对。加上 ClobberMemory 才是真实成本。
    DoNotOptimize(transfer.data()[0]);
    ClobberMemory();
  }
  return encoded;
}

/// RX 解码：从一个已聚合好的传输里逐帧拆出来。
std::uint64_t BenchDecode(std::uint64_t frame_iterations, std::uint32_t frame_bytes,
                          std::uint32_t frames_per_transfer) {
  // 先用编码器造一个真实的多包传输，再反复解它。
  const std::size_t capacity =
      static_cast<std::size_t>(frames_per_transfer) *
          (rndis::kPacketMsgHeaderBytes + frame_bytes + 8) +
      64;
  std::vector<std::byte> transfer(capacity);
  const std::vector<std::byte> frame = MakeFrame(frame_bytes);

  std::uint32_t packed = 0;
  std::uint32_t transfer_bytes = 0;
  {
    rndis::PacketMessageWriter writer(
        transfer,
        {.max_transfer_bytes = static_cast<std::uint32_t>(capacity),
         .max_messages = frames_per_transfer,
         .alignment_bytes = 8},
        512);
    for (std::uint32_t i = 0; i < frames_per_transfer; ++i) {
      if (!writer.TryAppend(frame)) {
        break;
      }
      ++packed;
    }
    transfer_bytes = writer.Finish();
  }
  if (packed == 0) {
    return 0;
  }

  const std::span<const std::byte> payload{transfer.data(), transfer_bytes};
  std::uint64_t decoded = 0;
  while (decoded < frame_iterations) {
    rndis::PacketMessageReader reader(payload, rndis::kDefaultMtu + rndis::kEthernetHeaderBytes);
    std::span<const std::byte> out;
    while (reader.Next(out) == rndis::ReadOutcome::kFrame) {
      DoNotOptimize(out.data());
      ++decoded;
    }
  }
  return decoded;
}

/// 编码 + 解码往返，模拟「主机发出去、设备原样发回来」的完整 CPU 成本。
std::uint64_t BenchRoundTrip(std::uint64_t frame_iterations, std::uint32_t frame_bytes,
                             std::uint32_t frames_per_transfer) {
  const std::size_t capacity =
      static_cast<std::size_t>(frames_per_transfer) *
          (rndis::kPacketMsgHeaderBytes + frame_bytes + 8) +
      64;
  std::vector<std::byte> transfer(capacity);
  const std::vector<std::byte> frame = MakeFrame(frame_bytes);

  std::uint64_t processed = 0;
  while (processed < frame_iterations) {
    std::uint32_t transfer_bytes = 0;
    std::uint32_t packed = 0;
    {
      rndis::PacketMessageWriter writer(
          transfer,
          {.max_transfer_bytes = static_cast<std::uint32_t>(capacity),
           .max_messages = frames_per_transfer,
           .alignment_bytes = 8},
          512);
      for (std::uint32_t i = 0; i < frames_per_transfer; ++i) {
        if (!writer.TryAppend(frame)) {
          break;
        }
        ++packed;
      }
      transfer_bytes = writer.Finish();
    }
    if (packed == 0) {
      break;
    }
    ClobberMemory();  // 见 BenchEncode 里的说明：防止 memcpy 被跨迭代消除

    rndis::PacketMessageReader reader(std::span<const std::byte>{transfer.data(), transfer_bytes},
                                     rndis::kDefaultMtu + rndis::kEthernetHeaderBytes);
    std::span<const std::byte> out;
    while (reader.Next(out) == rndis::ReadOutcome::kFrame) {
      DoNotOptimize(out.data());
      ++processed;
    }
  }
  return processed;
}

}  // namespace

void RegisterRndisBenchmarks(Runner& runner) {
  constexpr std::uint64_t kFrameOps = 1'000'000;

  // 聚合度对每帧成本的影响 —— 这是 --max-transfer-kb 这个调优旋钮的依据。
  for (const std::uint32_t per_transfer : {1U, 4U, 10U}) {
    runner.Add("RNDIS 编码",
               std::format("1514 字节 × 每传输 {} 帧", per_transfer),
               Config{.ops_per_round = kFrameOps, .bytes_per_op = kFullFrameBytes},
               [per_transfer](std::uint64_t n) {
                 return BenchEncode(n, kFullFrameBytes, per_transfer, 1);
               });
  }
  runner.Add("RNDIS 编码", "64 字节 × 每传输 10 帧",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kSmallFrameBytes},
             [](std::uint64_t n) { return BenchEncode(n, kSmallFrameBytes, 10, 1); });
  // 对齐要求（PacketAlignmentFactor）会引入额外的 memset 与 MessageLength 回填。
  runner.Add("RNDIS 编码", "1514 字节 × 每传输 10 帧 × 对齐 128",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) { return BenchEncode(n, kFullFrameBytes, 10, 128); });

  // 解码刻意**不填 bytes_per_op**（于是吞吐列显示「—」）：解码器是**零拷贝**的，
  // 它只解析头部并返回指向传输缓冲内部的视图，一个字节都不搬。给它算「Gbps」
  // 会得到上万这种荒谬数字，且与编码的吞吐不可比 —— 编码那边是真的在 memcpy。
  for (const std::uint32_t per_transfer : {1U, 4U, 10U}) {
    runner.Add("RNDIS 解码（零拷贝）",
               std::format("1514 字节 × 每传输 {} 帧", per_transfer),
               Config{.ops_per_round = kFrameOps},
               [per_transfer](std::uint64_t n) {
                 return BenchDecode(n, kFullFrameBytes, per_transfer);
               });
  }
  runner.Add("RNDIS 解码（零拷贝）", "64 字节 × 每传输 10 帧",
             Config{.ops_per_round = kFrameOps},
             [](std::uint64_t n) { return BenchDecode(n, kSmallFrameBytes, 10); });

  runner.Add("RNDIS 编解码往返", "1514 字节 × 每传输 10 帧",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) { return BenchRoundTrip(n, kFullFrameBytes, 10); });
  runner.Add("RNDIS 编解码往返", "64 字节 × 每传输 10 帧",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kSmallFrameBytes},
             [](std::uint64_t n) { return BenchRoundTrip(n, kSmallFrameBytes, 10); });
}

}  // namespace tetherkitnext::bench
