// Encoding and decoding of REMOTE_NDIS_PACKET_MSG -- **the data hot path**.
//
// This is the only protocol code in the whole project that executes for every frame, therefore:
//   * Everything is inline in the header, so the compiler can inline it into the bridge layer's loop;
//   * It does not return std::expected (which would allocate a std::string), does not throw, and does not allocate memory;
//     errors are expressed via enums + counters;
//   * Everything is noexcept.
//
// The two directions have different shapes; be sure to tell them apart:
//
//   device -> host (PacketMessageReader):
//     One bulk IN transfer strings together 1~N PACKET_MSGs, traversed by stepping by MessageLength.
//     Trailing garbage must be tolerated (devices also use the trick of "padding one extra byte" to avoid a ZLP).
//
//   host -> device (PacketMessageWriter):
//     Aggregates multiple frames into one bulk OUT, subject to three constraints: the device's MaxTransferSize,
//     MaxPacketsPerMessage, and the per-packet alignment determined by PacketAlignmentFactor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/rndis/protocol.h"

namespace tetherkitnext::rndis {

// =============================================================================
// Decoding: device -> host
// =============================================================================

/// Result of traversing one bulk IN transfer.
enum class ReadOutcome : std::uint8_t {
  kFrame,           ///< A frame was successfully taken out.
  kEndOfTransfer,   ///< Normal end (there may be tolerated trailing padding).
  kMalformed,       ///< Illegal data encountered; parsing of this transfer must stop.
};

/// Specific reason for illegal data, for statistics and troubleshooting.
enum class MalformedReason : std::uint8_t {
  kNone,
  kNotPacketMessage,   ///< MessageType is not REMOTE_NDIS_PACKET_MSG.
  kMessageTooShort,    ///< MessageLength < 44.
  kMessageOverruns,    ///< MessageLength exceeds the remaining buffer.
  kDataOutOfBounds,    ///< DataOffset + DataLength exceeds MessageLength.
  kFrameTooShort,      ///< DataLength < 14; cannot fit the Ethernet header.
  kFrameTooLong,       ///< DataLength exceeds this machine's per-frame limit.
};

/// Extracts Ethernet frames one by one from a bulk IN transfer buffer.
///
/// Usage:
/// ```
/// PacketMessageReader reader(transfer_bytes, max_frame_bytes);
/// std::span<const std::byte> frame;
/// while (reader.Next(frame) == ReadOutcome::kFrame) {
///   ... process frame ...
/// }
/// ```
class PacketMessageReader {
 public:
  PacketMessageReader(std::span<const std::byte> transfer, std::uint32_t max_frame_bytes) noexcept
      : transfer_(transfer), max_frame_bytes_(max_frame_bytes) {}

  /// Takes the next frame. When kFrame is returned, `frame` points into the transfer, with no copy.
  [[nodiscard]] ReadOutcome Next(std::span<const std::byte>& frame) noexcept {
    const std::size_t remaining = transfer_.size() - cursor_;

    // Remaining bytes are less than a complete header: treat as trailing padding, end normally.
    //
    // This tolerance is **required**: RNDIS specifies that neither host nor device sends a ZLP; instead, when the transfer length
    // is exactly an integer multiple of the endpoint's max packet size, 1 extra byte is padded to make it a short packet. That byte falls outside all
    // MessageLengths; treating it as an error would give a false report once on every 2048-byte transfer.
    if (remaining < kPacketMsgHeaderBytes) {
      trailing_padding_bytes_ = static_cast<std::uint32_t>(remaining);
      return ReadOutcome::kEndOfTransfer;
    }

    const std::byte* message = transfer_.data() + cursor_;

    const std::uint32_t message_type = LoadLe32(message + kMessageTypeOffset);
    if (message_type != ToRaw(MessageType::kPacket)) [[unlikely]] {
      return Fail(MalformedReason::kNotPacketMessage);
    }

    const std::uint32_t message_length = LoadLe32(message + kMessageLengthOffset);
    if (message_length < kPacketMsgHeaderBytes) [[unlikely]] {
      return Fail(MalformedReason::kMessageTooShort);
    }
    if (message_length > remaining) [[unlikely]] {
      return Fail(MalformedReason::kMessageOverruns);
    }

    const std::uint32_t data_offset = LoadLe32(message + kPacketDataOffsetOffset);
    const std::uint32_t data_length = LoadLe32(message + kPacketDataLengthOffset);

    // Absolute offset = 8 + DataOffset (protocol.h rule 2).
    // Use 64-bit arithmetic to avoid overflow when the device reports huge values.
    const std::uint64_t data_begin = static_cast<std::uint64_t>(kOffsetFieldBase) + data_offset;
    const std::uint64_t data_end = data_begin + data_length;
    if (data_end > message_length) [[unlikely]] {
      return Fail(MalformedReason::kDataOutOfBounds);
    }
    if (data_length < kEthernetHeaderBytes) [[unlikely]] {
      return Fail(MalformedReason::kFrameTooShort);
    }
    if (data_length > max_frame_bytes_) [[unlikely]] {
      return Fail(MalformedReason::kFrameTooLong);
    }

    frame = transfer_.subspan(cursor_ + static_cast<std::size_t>(data_begin), data_length);
    cursor_ += message_length;
    ++frames_decoded_;
    return ReadOutcome::kFrame;
  }

  [[nodiscard]] std::uint32_t FramesDecoded() const noexcept { return frames_decoded_; }

  [[nodiscard]] MalformedReason Reason() const noexcept { return reason_; }

  /// Number of trailing bytes ignored as padding (normally 0 or 1).
  [[nodiscard]] std::uint32_t TrailingPaddingBytes() const noexcept {
    return trailing_padding_bytes_;
  }

  /// Number of bytes consumed, used to verify whether the whole transfer was parsed completely.
  [[nodiscard]] std::size_t BytesConsumed() const noexcept { return cursor_; }

 private:
  ReadOutcome Fail(MalformedReason reason) noexcept {
    reason_ = reason;
    return ReadOutcome::kMalformed;
  }

  std::span<const std::byte> transfer_;
  std::uint32_t max_frame_bytes_;
  std::size_t cursor_ = 0;
  std::uint32_t frames_decoded_ = 0;
  std::uint32_t trailing_padding_bytes_ = 0;
  MalformedReason reason_ = MalformedReason::kNone;
};

/// Message identifier corresponding to MalformedReason.
///
/// Returning an identifier rather than a ready-made string is to preserve constexpr -- the actual text must be rendered in the user's current
/// language, and callers fetch it with `Text(MalformedReasonMessage(r))`.
[[nodiscard]] constexpr Msg MalformedReasonMessage(MalformedReason reason) noexcept {
  switch (reason) {
    case MalformedReason::kNone:
      return Msg::kRndisMalformedNone;
    case MalformedReason::kNotPacketMessage:
      return Msg::kRndisMalformedNotPacketMessage;
    case MalformedReason::kMessageTooShort:
      return Msg::kRndisMalformedMessageTooShort;
    case MalformedReason::kMessageOverruns:
      return Msg::kRndisMalformedMessageOverruns;
    case MalformedReason::kDataOutOfBounds:
      return Msg::kRndisMalformedDataOutOfBounds;
    case MalformedReason::kFrameTooShort:
      return Msg::kRndisMalformedFrameTooShort;
    case MalformedReason::kFrameTooLong:
      return Msg::kRndisMalformedFrameTooLong;
  }
  return Msg::kRndisMalformedUnknown;
}

/// Readable name of MalformedReason (in the current language).
[[nodiscard]] inline std::string_view MalformedReasonName(MalformedReason reason) noexcept {
  return Text(MalformedReasonMessage(reason));
}

// =============================================================================
// Encoding: host -> device
// =============================================================================

/// Aggregates multiple Ethernet frames into one bulk OUT transfer.
///
/// Details of alignment handling (this is the easiest place to get wrong):
///   RNDIS specifies that "except for the last one, every PACKET_MSG's MessageLength includes the trailing
///   alignment padding; the last one does not include external padding". So when appending the **next** message, this implementation
///   goes back and enlarges the **previous** message's MessageLength to swallow the padding bytes in between --
///   this way the last message's MessageLength is naturally 44 + frame length, fully conforming to the spec.
class PacketMessageWriter {
 public:
  /// @param transfer      Output buffer (usually the buffer of a libusb transfer)
  /// @param limits        Aggregation limits negotiated with the device
  /// @param endpoint_max_packet  wMaxPacketSize of the bulk OUT endpoint, used to avoid a ZLP
  struct Limits {
    std::uint32_t max_transfer_bytes = 0;      ///< The device's MaxTransferSize.
    std::uint32_t max_messages = 1;            ///< The device's MaxPacketsPerMessage.
    std::uint32_t alignment_bytes = 1;         ///< 1 << PacketAlignmentFactor.
  };

  PacketMessageWriter(std::span<std::byte> transfer, const Limits& limits,
                      std::uint32_t endpoint_max_packet) noexcept
      : transfer_(transfer),
        endpoint_max_packet_(endpoint_max_packet),
        max_messages_(limits.max_messages == 0 ? 1 : limits.max_messages),
        alignment_bytes_(limits.alignment_bytes == 0 ? 1 : limits.alignment_bytes) {
    // The effective capacity is the smaller of "buffer size" and the "device's MaxTransferSize",
    // then 1 byte is reserved for the padding that may be needed to avoid a ZLP.
    const std::size_t device_limit = limits.max_transfer_bytes == 0
                                         ? transfer.size()
                                         : static_cast<std::size_t>(limits.max_transfer_bytes);
    const std::size_t usable = device_limit < transfer.size() ? device_limit : transfer.size();
    capacity_ = usable > kZlpPadReserve ? usable - kZlpPadReserve : 0;
  }

  /// Tries to append a frame. Returning false means this batch is full; the caller should submit first and then retry.
  ///
  /// The frame length must be >= 14; the caller is responsible for guaranteeing this (frames read from BPF always satisfy it).
  [[nodiscard]] bool TryAppend(std::span<const std::byte> frame) noexcept {
    if (message_count_ >= max_messages_) [[unlikely]] {
      return false;
    }

    const auto frame_length = static_cast<std::uint32_t>(frame.size());

    // If a message already exists, first compute how much padding must be inserted to satisfy alignment.
    const std::size_t aligned_cursor =
        message_count_ == 0 ? cursor_ : AlignUp<std::size_t>(cursor_, alignment_bytes_);
    const std::size_t needed = aligned_cursor + kPacketMsgHeaderBytes + frame_length;
    if (needed > capacity_) {
      return false;
    }

    // Attribute the padding bytes to the **previous** message's MessageLength (see the class documentation comment).
    if (aligned_cursor != cursor_) {
      const auto padding = static_cast<std::uint32_t>(aligned_cursor - cursor_);
      std::memset(transfer_.data() + cursor_, 0, padding);
      const std::uint32_t previous_length =
          LoadLe32(transfer_.data() + last_header_offset_ + kMessageLengthOffset);
      StoreLe32(transfer_.data() + last_header_offset_ + kMessageLengthOffset,
                previous_length + padding);
      cursor_ = aligned_cursor;
    }

    std::byte* message = transfer_.data() + cursor_;
    const std::uint32_t message_length = kPacketMsgHeaderBytes + frame_length;

    StoreLe32(message + kMessageTypeOffset, ToRaw(MessageType::kPacket));
    StoreLe32(message + kMessageLengthOffset, message_length);
    // DataOffset is 36 rather than 44 -- the base point is message start + 8.
    StoreLe32(message + kPacketDataOffsetOffset, kPacketInlineDataOffset);
    StoreLe32(message + kPacketDataLengthOffset, frame_length);
    // The remaining fields (OOB, per-packet info, VcHandle, Reserved) are always 0 in this project.
    std::memset(message + kPacketOobDataOffsetOffset, 0,
                kPacketMsgHeaderBytes - kPacketOobDataOffsetOffset);

    std::memcpy(message + kPacketMsgHeaderBytes, frame.data(), frame_length);

    last_header_offset_ = cursor_;
    cursor_ += message_length;
    payload_bytes_ += frame_length;
    ++message_count_;
    return true;
  }

  /// Finishes this batch, returning the number of bytes to submit to libusb.
  ///
  /// Appends 1 byte of 0x00 when necessary to avoid a ZLP: RNDIS explicitly requires the host **not to** send
  /// zero-length packets, and when the transfer length is exactly an integer multiple of the endpoint's wMaxPacketSize, the USB host controller
  /// needs a short packet to mark the end of the transfer. Linux's usbnet does exactly this -- the extra byte is outside
  /// all MessageLengths, and the device must tolerate trailing garbage.
  [[nodiscard]] std::uint32_t Finish() noexcept {
    if (cursor_ == 0) {
      return 0;
    }
    if (endpoint_max_packet_ != 0 && cursor_ % endpoint_max_packet_ == 0) {
      transfer_[cursor_] = std::byte{0};
      ++cursor_;
      zlp_padding_added_ = true;
    }
    return static_cast<std::uint32_t>(cursor_);
  }

  [[nodiscard]] std::uint32_t MessageCount() const noexcept { return message_count_; }

  /// Number of Ethernet frame payload bytes aggregated so far (excluding RNDIS headers and padding), used to count throughput.
  [[nodiscard]] std::uint64_t PayloadBytes() const noexcept { return payload_bytes_; }

  [[nodiscard]] bool ZlpPaddingAdded() const noexcept { return zlp_padding_added_; }

  [[nodiscard]] bool Empty() const noexcept { return message_count_ == 0; }

  /// The maximum frame length the current batch can still hold (0 means it cannot fit any frame).
  /// The caller can use it to avoid the fallback logic of "took out a frame only to find it does not fit".
  [[nodiscard]] std::uint32_t RemainingFrameCapacity() const noexcept {
    if (message_count_ >= max_messages_) {
      return 0;
    }
    const std::size_t aligned_cursor =
        message_count_ == 0 ? cursor_ : AlignUp<std::size_t>(cursor_, alignment_bytes_);
    const std::size_t overhead = aligned_cursor + kPacketMsgHeaderBytes;
    return overhead >= capacity_ ? 0 : static_cast<std::uint32_t>(capacity_ - overhead);
  }

 private:
  /// Number of bytes reserved for ZLP avoidance.
  static constexpr std::size_t kZlpPadReserve = 1;

  std::span<std::byte> transfer_;
  std::uint32_t endpoint_max_packet_;
  std::uint32_t max_messages_;
  std::uint32_t alignment_bytes_;
  std::size_t capacity_ = 0;

  std::size_t cursor_ = 0;
  std::size_t last_header_offset_ = 0;
  std::uint32_t message_count_ = 0;
  std::uint64_t payload_bytes_ = 0;
  bool zlp_padding_added_ = false;
};

}  // namespace tetherkitnext::rndis
