// REMOTE_NDIS_PACKET_MSG 编解码的单元测试 —— 数据热路径的正确性防线。
//
// 重点覆盖：
//   1. DataOffset 基准点是消息起始 +8（编码写 36，解码按 8+36 定位）；
//   2. 多包聚合的对齐规则：填充归入**上一个**消息的 MessageLength，
//      最后一个消息不含外部填充；
//   3. ZLP 规避：传输长度恰为端点最大包长整数倍时补 1 字节；
//   4. 解码必须容忍尾部填充（否则每个满传输都会误报一次）；
//   5. 全部畸形输入都被识别为 kMalformed 且不读越界。
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <doctest.h>

#include "language_guard.h"

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/rndis/packet_codec.h"
#include "tetherkitnext/rndis/protocol.h"

using namespace tetherkitnext;         // NOLINT(google-build-using-namespace)
using namespace tetherkitnext::rndis;  // NOLINT(google-build-using-namespace)

namespace {

constexpr std::uint32_t kMaxFrame = 2048;

/// 造一帧内容可自校验的以太帧。
std::vector<std::byte> MakeFrame(std::uint32_t length, std::uint8_t tag) {
  std::vector<std::byte> frame(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)};
  }
  return frame;
}

bool FrameMatches(std::span<const std::byte> frame, std::uint32_t length, std::uint8_t tag) {
  if (frame.size() != length) {
    return false;
  }
  for (std::uint32_t i = 0; i < length; ++i) {
    if (frame[i] != std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)}) {
      return false;
    }
  }
  return true;
}

/// 手工构造一个 PACKET_MSG（用于测解码器，不依赖编码器）。
void WritePacketMessage(std::span<std::byte> out, std::span<const std::byte> frame,
                        std::uint32_t data_offset_field, std::uint32_t message_length,
                        std::uint32_t data_length_field) {
  StoreLe32(out.data() + kMessageTypeOffset, ToRaw(MessageType::kPacket));
  StoreLe32(out.data() + kMessageLengthOffset, message_length);
  StoreLe32(out.data() + kPacketDataOffsetOffset, data_offset_field);
  StoreLe32(out.data() + kPacketDataLengthOffset, data_length_field);
  for (std::uint32_t i = kPacketOobDataOffsetOffset; i < kPacketMsgHeaderBytes; i += 4) {
    StoreLe32(out.data() + i, 0);
  }
  const std::size_t absolute = kOffsetFieldBase + data_offset_field;
  if (!frame.empty() && absolute + frame.size() <= out.size()) {
    std::memcpy(out.data() + absolute, frame.data(), frame.size());
  }
}

/// 收集一次传输里解出的所有帧（拷贝出来，便于断言）。
struct DecodeSummary {
  std::vector<std::vector<std::byte>> frames;
  ReadOutcome final_outcome = ReadOutcome::kEndOfTransfer;
  MalformedReason reason = MalformedReason::kNone;
  std::uint32_t trailing_padding = 0;
};

DecodeSummary DecodeAll(std::span<const std::byte> transfer,
                        std::uint32_t max_frame_bytes = kMaxFrame) {
  DecodeSummary summary;
  PacketMessageReader reader(transfer, max_frame_bytes);
  std::span<const std::byte> frame;
  while (true) {
    const ReadOutcome outcome = reader.Next(frame);
    if (outcome != ReadOutcome::kFrame) {
      summary.final_outcome = outcome;
      break;
    }
    summary.frames.emplace_back(frame.begin(), frame.end());
  }
  summary.reason = reader.Reason();
  summary.trailing_padding = reader.TrailingPaddingBytes();
  return summary;
}

}  // namespace

TEST_SUITE("rndis.packet_codec") {

// ---------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------

TEST_CASE("编码单帧：DataOffset 必须写 36") {
  std::array<std::byte, 4096> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 2048, .max_messages = 1, .alignment_bytes = 1},
                             512);

  const auto frame = MakeFrame(1514, 0x11);
  REQUIRE(writer.TryAppend(frame));
  const std::uint32_t total = writer.Finish();

  CHECK(writer.MessageCount() == 1);
  CHECK(writer.PayloadBytes() == 1514);
  CHECK(total == kPacketMsgHeaderBytes + 1514);

  CHECK(LoadLe32(transfer.data() + kMessageTypeOffset) == ToRaw(MessageType::kPacket));
  CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == kPacketMsgHeaderBytes + 1514);
  // ★ 关键断言：36，不是 44。
  CHECK(LoadLe32(transfer.data() + kPacketDataOffsetOffset) == 36);
  CHECK(LoadLe32(transfer.data() + kPacketDataLengthOffset) == 1514);
  // OOB / per-packet info / VcHandle / Reserved 必须全零。
  CHECK(LoadLe32(transfer.data() + kPacketOobDataOffsetOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketOobDataLengthOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketNumOobDataElementsOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketPerPacketInfoOffsetOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketPerPacketInfoLengthOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketVcHandleOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketReservedOffset) == 0);

  // 帧数据落在绝对偏移 8 + 36 = 44 处。
  CHECK(FrameMatches(std::span<const std::byte>{transfer}.subspan(44, 1514), 1514, 0x11));
}

TEST_CASE("编码受 MaxPacketsPerMessage 限制") {
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 2, .alignment_bytes = 1},
                             512);
  CHECK(writer.TryAppend(MakeFrame(64, 1)));
  CHECK(writer.TryAppend(MakeFrame(64, 2)));
  CHECK_FALSE(writer.TryAppend(MakeFrame(64, 3)));  // 超出包数上限
  CHECK(writer.MessageCount() == 2);
}

TEST_CASE("编码受设备 MaxTransferSize 限制") {
  std::array<std::byte, 8192> transfer{};
  // 只够放一个 44+64=108 字节的消息（再留 1 字节 ZLP 余量）。
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 150, .max_messages = 10, .alignment_bytes = 1},
                             512);
  CHECK(writer.TryAppend(MakeFrame(64, 1)));
  CHECK_FALSE(writer.TryAppend(MakeFrame(64, 2)));
  CHECK(writer.MessageCount() == 1);
}

TEST_CASE("编码空批次 Finish 返回 0") {
  std::array<std::byte, 1024> transfer{};
  PacketMessageWriter writer(transfer, {.max_transfer_bytes = 1024}, 512);
  CHECK(writer.Empty());
  CHECK(writer.Finish() == 0);
}

TEST_CASE("多包聚合：填充归入上一个消息，最后一个不含外部填充") {
  std::array<std::byte, 8192> transfer{};
  // 对齐 8 字节（PacketAlignmentFactor = 3）。
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 3, .alignment_bytes = 8},
                             512);

  // 帧长 61 → 消息长 44+61 = 105。105 不是 8 的倍数，向上对齐到 112（14×8），
  // 所以下一个消息前需要 7 字节填充，且这 7 字节要被吞进**第一个**消息的
  // MessageLength。
  REQUIRE(writer.TryAppend(MakeFrame(61, 0xA0)));
  REQUIRE(writer.TryAppend(MakeFrame(70, 0xB0)));
  REQUIRE(writer.TryAppend(MakeFrame(64, 0xC0)));
  const std::uint32_t total = writer.Finish();

  // 第一个消息：105 → 被扩到 112（吞掉 7 字节填充）。
  CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == 112);
  CHECK(112 % 8 == 0);

  // 第二个消息从 112 开始：44+70 = 114 → 累计 226，向上对齐到 232，
  // 于是第二个消息的 MessageLength 被扩到 114 + 6 = 120。
  CHECK(LoadLe32(transfer.data() + 112 + kMessageLengthOffset) == 120);
  CHECK((112 + 120) % 8 == 0);

  // 第三个（最后一个）消息从 232 开始：44+64 = 108，**不含**外部填充。
  CHECK(LoadLe32(transfer.data() + 232 + kMessageLengthOffset) == 108);
  CHECK(total == 232 + 108);
  CHECK(writer.PayloadBytes() == 61 + 70 + 64);
}

TEST_CASE("多包聚合：编码结果能被解码器完整还原") {
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 4, .alignment_bytes = 8},
                             512);
  const std::array<std::uint32_t, 4> lengths{61, 70, 1514, 14};
  const std::array<std::uint8_t, 4> tags{0xA0, 0xB0, 0xC0, 0xD0};
  for (std::size_t i = 0; i < lengths.size(); ++i) {
    REQUIRE(writer.TryAppend(MakeFrame(lengths[i], tags[i])));
  }
  const std::uint32_t total = writer.Finish();

  const auto summary = DecodeAll(std::span<const std::byte>{transfer}.first(total));
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  REQUIRE(summary.frames.size() == 4);
  for (std::size_t i = 0; i < lengths.size(); ++i) {
    CHECK(FrameMatches(summary.frames[i], lengths[i], tags[i]));
  }
}

TEST_CASE("ZLP 规避：长度恰为端点最大包长整数倍时补 1 字节") {
  std::array<std::byte, 4096> transfer{};
  constexpr std::uint32_t kEndpointMaxPacket = 512;
  // 让总长恰好是 512：44 + 468 = 512。
  PacketMessageWriter writer(
      transfer, {.max_transfer_bytes = 4096, .max_messages = 1, .alignment_bytes = 1},
      kEndpointMaxPacket);
  REQUIRE(writer.TryAppend(MakeFrame(468, 0x55)));
  const std::uint32_t total = writer.Finish();

  CHECK(writer.ZlpPaddingAdded());
  CHECK(total == 513);
  CHECK(total % kEndpointMaxPacket != 0);
  // 补的那个字节必须是 0，且在 MessageLength（512）之外。
  CHECK(transfer[512] == std::byte{0});
  CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == 512);
}

TEST_CASE("ZLP 规避：长度不是整数倍时不补字节") {
  std::array<std::byte, 4096> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 4096, .max_messages = 1, .alignment_bytes = 1},
                             512);
  REQUIRE(writer.TryAppend(MakeFrame(100, 0x66)));
  const std::uint32_t total = writer.Finish();
  CHECK_FALSE(writer.ZlpPaddingAdded());
  CHECK(total == kPacketMsgHeaderBytes + 100);
}

TEST_CASE("RemainingFrameCapacity 报告准确，可避免取帧后回退") {
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 200, .max_messages = 5, .alignment_bytes = 1},
                             512);
  // 容量 200 - 1（ZLP 余量）= 199；减去 44 字节头 → 155。
  CHECK(writer.RemainingFrameCapacity() == 155);
  REQUIRE(writer.TryAppend(MakeFrame(100, 1)));
  // 已用 144；剩 199 - 144 - 44 = 11。
  CHECK(writer.RemainingFrameCapacity() == 11);
  // 报告说只能再放 11 字节，那么放 12 字节必须失败。
  CHECK_FALSE(writer.TryAppend(MakeFrame(12, 2)));
  CHECK(writer.TryAppend(MakeFrame(11, 2)));
  CHECK(writer.RemainingFrameCapacity() == 0);
}

TEST_CASE("包数用满后 RemainingFrameCapacity 归零") {
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 1, .alignment_bytes = 1},
                             512);
  REQUIRE(writer.TryAppend(MakeFrame(64, 1)));
  CHECK(writer.RemainingFrameCapacity() == 0);
}

// ---------------------------------------------------------------------------
// 解码
// ---------------------------------------------------------------------------

TEST_CASE("解码单帧：按 8 + DataOffset 定位数据") {
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 1514);
  const auto frame = MakeFrame(1514, 0x22);
  WritePacketMessage(transfer, frame, kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 1514, 1514);

  const auto summary = DecodeAll(transfer);
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  REQUIRE(summary.frames.size() == 1);
  CHECK(FrameMatches(summary.frames[0], 1514, 0x22));
}

TEST_CASE("解码：数据不紧跟头部（DataOffset 大于 36）") {
  // 设备完全可以在头部与数据之间留空隙，解码器必须按 DataOffset 走。
  constexpr std::uint32_t kGap = 16;
  constexpr std::uint32_t kFrameLength = 100;
  const std::uint32_t message_length = kPacketMsgHeaderBytes + kGap + kFrameLength;
  std::vector<std::byte> transfer(message_length);
  const auto frame = MakeFrame(kFrameLength, 0x33);
  WritePacketMessage(transfer, frame, kPacketInlineDataOffset + kGap, message_length, kFrameLength);

  const auto summary = DecodeAll(transfer);
  REQUIRE(summary.frames.size() == 1);
  CHECK(FrameMatches(summary.frames[0], kFrameLength, 0x33));
}

TEST_CASE("解码：容忍尾部 1 字节 ZLP 规避填充") {
  // 这是必须的容忍 —— 否则每个满传输都会误报一次畸形。
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64 + 1);
  WritePacketMessage(transfer, MakeFrame(64, 0x44), kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 64, 64);

  const auto summary = DecodeAll(transfer);
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  CHECK(summary.reason == MalformedReason::kNone);
  REQUIRE(summary.frames.size() == 1);
  CHECK(summary.trailing_padding == 1);
}

TEST_CASE("解码：容忍最多 43 字节的尾部垃圾") {
  // 少于一个完整头部（44 字节）的尾部一律当填充。
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64 + 43);
  WritePacketMessage(transfer, MakeFrame(64, 0x45), kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 64, 64);
  // 尾部填成非零垃圾。
  for (std::size_t i = transfer.size() - 43; i < transfer.size(); ++i) {
    transfer[i] = std::byte{0xEE};
  }
  const auto summary = DecodeAll(transfer);
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  REQUIRE(summary.frames.size() == 1);
  CHECK(summary.trailing_padding == 43);
}

TEST_CASE("解码空传输") {
  const auto summary = DecodeAll({});
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  CHECK(summary.frames.empty());
}

TEST_CASE("解码畸形输入：全部被识别且不读越界") {
  SUBCASE("MessageType 不是 PACKET_MSG") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset,
                       kPacketMsgHeaderBytes + 64, 64);
    StoreLe32(transfer.data() + kMessageTypeOffset, ToRaw(MessageType::kKeepAlive));
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kNotPacketMessage);
  }

  SUBCASE("MessageLength 小于 44") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset, 40, 64);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kMessageTooShort);
  }

  SUBCASE("MessageLength 超出剩余缓冲") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset, 0xFFFF'FFFFU, 64);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kMessageOverruns);
  }

  SUBCASE("DataOffset + DataLength 越界") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, {}, kPacketInlineDataOffset, kPacketMsgHeaderBytes + 64, 1000);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kDataOutOfBounds);
  }

  SUBCASE("DataOffset 巨大值不得整数溢出") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, {}, 0xFFFF'FFF8U, kPacketMsgHeaderBytes + 64, 64);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kDataOutOfBounds);
  }

  SUBCASE("DataLength 小于以太头长度") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(13, 1), kPacketInlineDataOffset,
                       kPacketMsgHeaderBytes + 64, 13);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kFrameTooShort);
  }

  SUBCASE("DataLength 超过本机单帧上限") {
    const std::uint32_t message_length = kPacketMsgHeaderBytes + 3000;
    std::vector<std::byte> transfer(message_length);
    WritePacketMessage(transfer, {}, kPacketInlineDataOffset, message_length, 3000);
    const auto summary = DecodeAll(transfer, 2048);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kFrameTooLong);
  }
}

TEST_CASE("解码：前几帧有效、中途畸形时保留已解出的帧") {
  // 真实设备出 bug 时的行为要求：不能因为第 3 个消息坏了就丢掉前 2 帧。
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 3, .alignment_bytes = 1},
                             512);
  REQUIRE(writer.TryAppend(MakeFrame(64, 0x01)));
  REQUIRE(writer.TryAppend(MakeFrame(64, 0x02)));
  REQUIRE(writer.TryAppend(MakeFrame(64, 0x03)));
  const std::uint32_t total = writer.Finish();

  // 破坏第三个消息的类型字段。
  // 对齐 1 字节时消息紧邻排布，第三个消息的偏移就是前两个之和。
  const std::size_t third_offset = std::size_t{2} * (kPacketMsgHeaderBytes + 64);
  StoreLe32(transfer.data() + third_offset + kMessageTypeOffset, 0xDEAD'BEEFU);

  const auto summary = DecodeAll(std::span<const std::byte>{transfer}.first(total));
  CHECK(summary.final_outcome == ReadOutcome::kMalformed);
  CHECK(summary.reason == MalformedReason::kNotPacketMessage);
  REQUIRE(summary.frames.size() == 2);
  CHECK(FrameMatches(summary.frames[0], 64, 0x01));
  CHECK(FrameMatches(summary.frames[1], 64, 0x02));
}

TEST_CASE("解码：帧视图指向传输缓冲内部，无拷贝") {
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 100);
  WritePacketMessage(transfer, MakeFrame(100, 0x77), kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 100, 100);

  PacketMessageReader reader(transfer, kMaxFrame);
  std::span<const std::byte> frame;
  REQUIRE(reader.Next(frame) == ReadOutcome::kFrame);
  // 零拷贝的证明：帧指针落在传输缓冲内部的绝对偏移 44 处。
  CHECK(frame.data() == transfer.data() + 44);
  CHECK(reader.FramesDecoded() == 1);
  CHECK(reader.BytesConsumed() == kPacketMsgHeaderBytes + 100);
}

TEST_CASE("MalformedReason 名字完整") {
  // 名字是面向用户的文案，会随语言变化，所以断言前先把语言钉死。
  const tetherkitnext::testing::ScopedLanguage guard{tetherkitnext::Language::kChinese};
  CHECK(MalformedReasonName(MalformedReason::kNone) == "无");
  CHECK_FALSE(MalformedReasonName(MalformedReason::kNotPacketMessage).empty());
  CHECK_FALSE(MalformedReasonName(MalformedReason::kDataOutOfBounds).empty());
  CHECK_FALSE(MalformedReasonName(MalformedReason::kFrameTooLong).empty());
}

// ---------------------------------------------------------------------------
// 往返性质测试
// ---------------------------------------------------------------------------

TEST_CASE("往返：各种对齐因子与帧长组合都能无损还原") {
  std::array<std::byte, 16384> transfer{};
  for (std::uint32_t factor = 0; factor <= kMaxPacketAlignmentFactor; ++factor) {
    const std::uint32_t alignment = 1U << factor;
    for (const std::uint32_t length : {14U, 15U, 63U, 64U, 65U, 127U, 128U, 1514U}) {
      PacketMessageWriter writer(
          transfer,
          {.max_transfer_bytes = 16384, .max_messages = 4, .alignment_bytes = alignment}, 512);

      std::uint32_t appended = 0;
      for (std::uint8_t i = 0; i < 4; ++i) {
        if (!writer.TryAppend(MakeFrame(length, static_cast<std::uint8_t>(0x10 + i)))) {
          break;
        }
        ++appended;
      }
      REQUIRE(appended > 0);
      const std::uint32_t total = writer.Finish();

      const auto summary = DecodeAll(std::span<const std::byte>{transfer}.first(total));
      CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
      REQUIRE(summary.frames.size() == appended);
      for (std::uint32_t i = 0; i < appended; ++i) {
        CHECK(FrameMatches(summary.frames[i], length, static_cast<std::uint8_t>(0x10 + i)));
      }
    }
  }
}

}  // TEST_SUITE("rndis.packet_codec")
