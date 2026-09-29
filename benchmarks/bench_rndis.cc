// Microbenchmarks of RNDIS encoding/decoding.
//
// These two paths are walked once per frame, and are the only CPU cost billed per frame besides system calls:
//   TX: PacketMessageWriter wraps Ethernet frames into RNDIS_PACKET_MSG (including multi-packet aggregation and alignment)
//   RX: PacketMessageReader unpacks frame by frame from one bulk IN transfer
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

/// TX encoding: aggregates `frames_per_transfer` frames into one transfer.
///
/// The unit of measurement is the **frame** rather than the transfer, so numbers at different aggregation levels can be compared directly side by side.
std::uint64_t BenchEncode(std::uint64_t frame_iterations, std::uint32_t frame_bytes,
                          std::uint32_t frames_per_transfer, std::uint32_t alignment_bytes) {
  // The transfer buffer must hold frames_per_transfer full frames.
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
    // * The **contents of the target buffer** must also be forcibly observed. If only DoNotOptimize(the return value of Finish()),
    // the compiler sees that nobody reads this buffer afterwards and eliminates the memcpy inside TryAppend
    // entirely -- so the measurement comes out at 14.5 ns/frame, faster than a bare 1514-byte memcpy (25 ns),
    // which is obviously wrong at a glance. Adding ClobberMemory gives the true cost.
    DoNotOptimize(transfer.data()[0]);
    ClobberMemory();
  }
  return encoded;
}

/// RX decoding: unpacks frame by frame from an already-aggregated transfer.
std::uint64_t BenchDecode(std::uint64_t frame_iterations, std::uint32_t frame_bytes,
                          std::uint32_t frames_per_transfer) {
  // First use the encoder to build a realistic multi-packet transfer, then decode it repeatedly.
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

/// Encode + decode round trip, simulating the full CPU cost of "the host sends out, the device sends it back unchanged".
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
    ClobberMemory();  // see the explanation in BenchEncode: prevents the memcpy from being eliminated across iterations

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

  // The effect of aggregation level on per-frame cost -- this is the basis of the --max-transfer-kb tuning knob.
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
  // The alignment requirement (PacketAlignmentFactor) introduces extra memset and MessageLength backfilling.
  runner.Add("RNDIS 编码", "1514 字节 × 每传输 10 帧 × 对齐 128",
             Config{.ops_per_round = kFrameOps, .bytes_per_op = kFullFrameBytes},
             [](std::uint64_t n) { return BenchEncode(n, kFullFrameBytes, 10, 128); });

  // Decoding deliberately **does not fill bytes_per_op** (so the throughput column shows "-"): the decoder is **zero-copy**,
  // it only parses headers and returns views pointing into the transfer buffer, moving not a single byte. Computing "Gbps" for it
  // would give absurd numbers in the tens of thousands, not comparable with the encoding's throughput -- on the encoding side memcpy really happens.
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
