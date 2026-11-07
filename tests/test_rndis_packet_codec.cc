// Unit tests of REMOTE_NDIS_PACKET_MSG encoding/decoding -- the line of defense for data hot path correctness.
//
// Key coverage:
//   1. The DataOffset base point is message start +8 (encoding writes 36, decoding locates by 8+36);
//   2. Alignment rules for multi-packet aggregation: padding is attributed to the **previous** message's MessageLength,
//      and the last message has no external padding;
//   3. ZLP avoidance: pad 1 byte when the transfer length is exactly an integer multiple of the endpoint's max packet size;
//   4. Decoding must tolerate trailing padding (otherwise every full transfer would give a false report once);
//   5. All malformed inputs are recognized as kMalformed without reading out of bounds.
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

/// Builds an Ethernet frame whose content is self-verifying.
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

/// Hand-builds a PACKET_MSG (for testing the decoder, independent of the encoder).
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

/// Collects all frames decoded from one transfer (copied out, to ease assertions).
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
// Encoding
// ---------------------------------------------------------------------------

TEST_CASE("encode one frame: DataOffset is 36") {
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
  // * Key assertion: 36, not 44.
  CHECK(LoadLe32(transfer.data() + kPacketDataOffsetOffset) == 36);
  CHECK(LoadLe32(transfer.data() + kPacketDataLengthOffset) == 1514);
  // OOB / per-packet info / VcHandle / Reserved must be all zero.
  CHECK(LoadLe32(transfer.data() + kPacketOobDataOffsetOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketOobDataLengthOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketNumOobDataElementsOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketPerPacketInfoOffsetOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketPerPacketInfoLengthOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketVcHandleOffset) == 0);
  CHECK(LoadLe32(transfer.data() + kPacketReservedOffset) == 0);

  // The frame data lands at absolute offset 8 + 36 = 44.
  CHECK(FrameMatches(std::span<const std::byte>{transfer}.subspan(44, 1514), 1514, 0x11));
}

TEST_CASE("encoding respects MaxPacketsPerMessage") {
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 2, .alignment_bytes = 1},
                             512);
  CHECK(writer.TryAppend(MakeFrame(64, 1)));
  CHECK(writer.TryAppend(MakeFrame(64, 2)));
  CHECK_FALSE(writer.TryAppend(MakeFrame(64, 3)));  // exceeds the packet count limit
  CHECK(writer.MessageCount() == 2);
}

TEST_CASE("encoding respects the device's MaxTransferSize") {
  std::array<std::byte, 8192> transfer{};
  // Only enough room for one 44+64=108-byte message (leaving 1 byte of ZLP margin).
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 150, .max_messages = 10, .alignment_bytes = 1},
                             512);
  CHECK(writer.TryAppend(MakeFrame(64, 1)));
  CHECK_FALSE(writer.TryAppend(MakeFrame(64, 2)));
  CHECK(writer.MessageCount() == 1);
}

TEST_CASE("Finish on an empty batch returns 0") {
  std::array<std::byte, 1024> transfer{};
  PacketMessageWriter writer(transfer, {.max_transfer_bytes = 1024}, 512);
  CHECK(writer.Empty());
  CHECK(writer.Finish() == 0);
}

TEST_CASE("aggregation: padding belongs to the previous message, the last has no trailing padding") {
  std::array<std::byte, 8192> transfer{};
  // Align to 8 bytes (PacketAlignmentFactor = 3).
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 3, .alignment_bytes = 8},
                             512);

  // Frame length 61 -> message length 44+61 = 105. 105 is not a multiple of 8, and rounds up to 112 (14x8),
  // so 7 bytes of padding are needed before the next message, and those 7 bytes must be swallowed into the **first** message's
  // MessageLength.
  REQUIRE(writer.TryAppend(MakeFrame(61, 0xA0)));
  REQUIRE(writer.TryAppend(MakeFrame(70, 0xB0)));
  REQUIRE(writer.TryAppend(MakeFrame(64, 0xC0)));
  const std::uint32_t total = writer.Finish();

  // The first message: 105 -> extended to 112 (swallowing 7 bytes of padding).
  CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == 112);
  CHECK(112 % 8 == 0);

  // The second message starts at 112: 44+70 = 114 -> cumulative 226, rounded up to 232,
  // so the second message's MessageLength is extended to 114 + 6 = 120.
  CHECK(LoadLe32(transfer.data() + 112 + kMessageLengthOffset) == 120);
  CHECK((112 + 120) % 8 == 0);

  // The third (last) message starts at 232: 44+64 = 108, **without** external padding.
  CHECK(LoadLe32(transfer.data() + 232 + kMessageLengthOffset) == 108);
  CHECK(total == 232 + 108);
  CHECK(writer.PayloadBytes() == 61 + 70 + 64);
}

TEST_CASE("aggregation: the decoder restores what was encoded") {
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

TEST_CASE("ZLP avoidance: one byte is added when the length is a multiple of the max packet size") {
  std::array<std::byte, 4096> transfer{};
  constexpr std::uint32_t kEndpointMaxPacket = 512;
  // Make the total length exactly 512: 44 + 468 = 512.
  PacketMessageWriter writer(
      transfer, {.max_transfer_bytes = 4096, .max_messages = 1, .alignment_bytes = 1},
      kEndpointMaxPacket);
  REQUIRE(writer.TryAppend(MakeFrame(468, 0x55)));
  const std::uint32_t total = writer.Finish();

  CHECK(writer.ZlpPaddingAdded());
  CHECK(total == 513);
  CHECK(total % kEndpointMaxPacket != 0);
  // The padded byte must be 0, and outside the MessageLength (512).
  CHECK(transfer[512] == std::byte{0});
  CHECK(LoadLe32(transfer.data() + kMessageLengthOffset) == 512);
}

TEST_CASE("ZLP avoidance: nothing is added otherwise") {
  std::array<std::byte, 4096> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 4096, .max_messages = 1, .alignment_bytes = 1},
                             512);
  REQUIRE(writer.TryAppend(MakeFrame(100, 0x66)));
  const std::uint32_t total = writer.Finish();
  CHECK_FALSE(writer.ZlpPaddingAdded());
  CHECK(total == kPacketMsgHeaderBytes + 100);
}

TEST_CASE("RemainingFrameCapacity is exact, so frames never need to be put back") {
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 200, .max_messages = 5, .alignment_bytes = 1},
                             512);
  // Capacity 200 - 1 (ZLP margin) = 199; minus the 44-byte header -> 155.
  CHECK(writer.RemainingFrameCapacity() == 155);
  REQUIRE(writer.TryAppend(MakeFrame(100, 1)));
  // 144 used; 199 - 144 - 44 = 11 left.
  CHECK(writer.RemainingFrameCapacity() == 11);
  // The report says only 11 more bytes fit, so putting in 12 bytes must fail.
  CHECK_FALSE(writer.TryAppend(MakeFrame(12, 2)));
  CHECK(writer.TryAppend(MakeFrame(11, 2)));
  CHECK(writer.RemainingFrameCapacity() == 0);
}

TEST_CASE("RemainingFrameCapacity is 0 once the packet count is used up") {
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 1, .alignment_bytes = 1},
                             512);
  REQUIRE(writer.TryAppend(MakeFrame(64, 1)));
  CHECK(writer.RemainingFrameCapacity() == 0);
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

TEST_CASE("decode one frame: data is at 8 + DataOffset") {
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 1514);
  const auto frame = MakeFrame(1514, 0x22);
  WritePacketMessage(transfer, frame, kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 1514, 1514);

  const auto summary = DecodeAll(transfer);
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  REQUIRE(summary.frames.size() == 1);
  CHECK(FrameMatches(summary.frames[0], 1514, 0x22));
}

TEST_CASE("decode: data not right after the header (DataOffset above 36)") {
  // The device is entirely free to leave a gap between the header and the data; the decoder must follow DataOffset.
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

TEST_CASE("decode: tolerates 1 trailing byte of ZLP padding") {
  // This tolerance is required -- otherwise every full transfer would falsely report one malformed message.
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64 + 1);
  WritePacketMessage(transfer, MakeFrame(64, 0x44), kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 64, 64);

  const auto summary = DecodeAll(transfer);
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  CHECK(summary.reason == MalformedReason::kNone);
  REQUIRE(summary.frames.size() == 1);
  CHECK(summary.trailing_padding == 1);
}

TEST_CASE("decode: tolerates up to 43 trailing garbage bytes") {
  // A tail shorter than one complete header (44 bytes) is always treated as padding.
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64 + 43);
  WritePacketMessage(transfer, MakeFrame(64, 0x45), kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 64, 64);
  // Fill the tail with non-zero garbage.
  for (std::size_t i = transfer.size() - 43; i < transfer.size(); ++i) {
    transfer[i] = std::byte{0xEE};
  }
  const auto summary = DecodeAll(transfer);
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  REQUIRE(summary.frames.size() == 1);
  CHECK(summary.trailing_padding == 43);
}

TEST_CASE("decode an empty transfer") {
  const auto summary = DecodeAll({});
  CHECK(summary.final_outcome == ReadOutcome::kEndOfTransfer);
  CHECK(summary.frames.empty());
}

TEST_CASE("decode malformed input: every case is detected without reading out of bounds") {
  SUBCASE("MessageType is not PACKET_MSG") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset,
                       kPacketMsgHeaderBytes + 64, 64);
    StoreLe32(transfer.data() + kMessageTypeOffset, ToRaw(MessageType::kKeepAlive));
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kNotPacketMessage);
  }

  SUBCASE("MessageLength below 44") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset, 40, 64);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kMessageTooShort);
  }

  SUBCASE("MessageLength beyond the remaining buffer") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(64, 1), kPacketInlineDataOffset, 0xFFFF'FFFFU, 64);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kMessageOverruns);
  }

  SUBCASE("DataOffset + DataLength out of bounds") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, {}, kPacketInlineDataOffset, kPacketMsgHeaderBytes + 64, 1000);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kDataOutOfBounds);
  }

  SUBCASE("a huge DataOffset must not overflow") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, {}, 0xFFFF'FFF8U, kPacketMsgHeaderBytes + 64, 64);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kDataOutOfBounds);
  }

  SUBCASE("DataLength shorter than an Ethernet header") {
    std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 64);
    WritePacketMessage(transfer, MakeFrame(13, 1), kPacketInlineDataOffset,
                       kPacketMsgHeaderBytes + 64, 13);
    const auto summary = DecodeAll(transfer);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kFrameTooShort);
  }

  SUBCASE("DataLength above this host's frame limit") {
    const std::uint32_t message_length = kPacketMsgHeaderBytes + 3000;
    std::vector<std::byte> transfer(message_length);
    WritePacketMessage(transfer, {}, kPacketInlineDataOffset, message_length, 3000);
    const auto summary = DecodeAll(transfer, 2048);
    CHECK(summary.final_outcome == ReadOutcome::kMalformed);
    CHECK(summary.reason == MalformedReason::kFrameTooLong);
  }
}

TEST_CASE("decode: frames decoded before a malformed one are kept") {
  // Required behavior when a real device has a bug: the first 2 frames must not be dropped just because the 3rd message is bad.
  std::array<std::byte, 8192> transfer{};
  PacketMessageWriter writer(transfer,
                             {.max_transfer_bytes = 8192, .max_messages = 3, .alignment_bytes = 1},
                             512);
  REQUIRE(writer.TryAppend(MakeFrame(64, 0x01)));
  REQUIRE(writer.TryAppend(MakeFrame(64, 0x02)));
  REQUIRE(writer.TryAppend(MakeFrame(64, 0x03)));
  const std::uint32_t total = writer.Finish();

  // Corrupt the type field of the third message.
  // With 1-byte alignment messages are laid out back to back, and the third message's offset is the sum of the first two.
  const std::size_t third_offset = std::size_t{2} * (kPacketMsgHeaderBytes + 64);
  StoreLe32(transfer.data() + third_offset + kMessageTypeOffset, 0xDEAD'BEEFU);

  const auto summary = DecodeAll(std::span<const std::byte>{transfer}.first(total));
  CHECK(summary.final_outcome == ReadOutcome::kMalformed);
  CHECK(summary.reason == MalformedReason::kNotPacketMessage);
  REQUIRE(summary.frames.size() == 2);
  CHECK(FrameMatches(summary.frames[0], 64, 0x01));
  CHECK(FrameMatches(summary.frames[1], 64, 0x02));
}

TEST_CASE("decode: frame views point into the transfer buffer, no copy") {
  std::vector<std::byte> transfer(kPacketMsgHeaderBytes + 100);
  WritePacketMessage(transfer, MakeFrame(100, 0x77), kPacketInlineDataOffset,
                     kPacketMsgHeaderBytes + 100, 100);

  PacketMessageReader reader(transfer, kMaxFrame);
  std::span<const std::byte> frame;
  REQUIRE(reader.Next(frame) == ReadOutcome::kFrame);
  // Proof of zero-copy: the frame pointer lands at absolute offset 44 inside the transfer buffer.
  CHECK(frame.data() == transfer.data() + 44);
  CHECK(reader.FramesDecoded() == 1);
  CHECK(reader.BytesConsumed() == kPacketMsgHeaderBytes + 100);
}

TEST_CASE("every MalformedReason has a name") {
  // The name is user-facing text that changes with language, so pin the language before asserting.
  const tetherkitnext::testing::ScopedLanguage guard{tetherkitnext::Language::kEnglish};
  CHECK(MalformedReasonName(MalformedReason::kNone) == "none");
  CHECK_FALSE(MalformedReasonName(MalformedReason::kNotPacketMessage).empty());
  CHECK_FALSE(MalformedReasonName(MalformedReason::kDataOutOfBounds).empty());
  CHECK_FALSE(MalformedReasonName(MalformedReason::kFrameTooLong).empty());
}

// ---------------------------------------------------------------------------
// Round-trip property tests
// ---------------------------------------------------------------------------

TEST_CASE("round trip: every alignment factor and frame length survives losslessly") {
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
