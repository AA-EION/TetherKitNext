// REMOTE_NDIS_PACKET_MSG 的编解码 —— **数据热路径**。
//
// 这是全项目唯一每帧都要执行的协议代码，因此：
//   * 全部 inline 放在头文件里，让编译器能内联进桥接层的循环；
//   * 不返回 std::expected（会分配 std::string）、不抛异常、不分配内存；
//     错误通过枚举 + 计数器表达；
//   * 全部 noexcept。
//
// 两个方向的形态不同，务必分清：
//
//   device → host（PacketMessageReader）：
//     一次 bulk IN 传输里串着 1~N 个 PACKET_MSG，按 MessageLength 步进遍历。
//     必须容忍尾部垃圾（设备也会用「多补 1 字节」的手法规避 ZLP）。
//
//   host → device（PacketMessageWriter）：
//     把多帧聚合进一次 bulk OUT，受三重约束：设备的 MaxTransferSize、
//     MaxPacketsPerMessage、以及 PacketAlignmentFactor 决定的每包对齐。
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
// 解码：device → host
// =============================================================================

/// 遍历一次 bulk IN 传输的结果。
enum class ReadOutcome : std::uint8_t {
  kFrame,           ///< 成功取出一帧。
  kEndOfTransfer,   ///< 正常结束（可能有被容忍的尾部填充）。
  kMalformed,       ///< 遇到非法数据，必须停止解析本次传输。
};

/// 非法数据的具体原因，用于统计与排障。
enum class MalformedReason : std::uint8_t {
  kNone,
  kNotPacketMessage,   ///< MessageType 不是 REMOTE_NDIS_PACKET_MSG。
  kMessageTooShort,    ///< MessageLength < 44。
  kMessageOverruns,    ///< MessageLength 超出剩余缓冲。
  kDataOutOfBounds,    ///< DataOffset + DataLength 超出 MessageLength。
  kFrameTooShort,      ///< DataLength < 14，装不下以太头。
  kFrameTooLong,       ///< DataLength 超过本机允许的单帧上限。
};

/// 从一次 bulk IN 传输缓冲里逐帧取出以太帧。
///
/// 用法：
/// ```
/// PacketMessageReader reader(transfer_bytes, max_frame_bytes);
/// std::span<const std::byte> frame;
/// while (reader.Next(frame) == ReadOutcome::kFrame) {
///   ... 处理 frame ...
/// }
/// ```
class PacketMessageReader {
 public:
  PacketMessageReader(std::span<const std::byte> transfer, std::uint32_t max_frame_bytes) noexcept
      : transfer_(transfer), max_frame_bytes_(max_frame_bytes) {}

  /// 取下一帧。返回 kFrame 时 `frame` 指向 transfer 内部，无拷贝。
  [[nodiscard]] ReadOutcome Next(std::span<const std::byte>& frame) noexcept {
    const std::size_t remaining = transfer_.size() - cursor_;

    // 剩余不足一个完整头部：按尾部填充处理，正常结束。
    //
    // 这是**必须**的容忍：RNDIS 规定 host 与 device 都不发 ZLP，而是在传输长度
    // 恰为端点最大包长整数倍时多补 1 个字节使其变成短包。那个字节落在所有
    // MessageLength 之外，若把它当错误就会在每 2048 字节的传输上误报一次。
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

    // 绝对偏移 = 8 + DataOffset（protocol.h 规则 2）。
    // 用 64 位运算避免设备汇报巨大值时溢出。
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

  /// 被当作填充忽略掉的尾部字节数（正常应为 0 或 1）。
  [[nodiscard]] std::uint32_t TrailingPaddingBytes() const noexcept {
    return trailing_padding_bytes_;
  }

  /// 已消费的字节数，用于校验是否把整个传输都解析完了。
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

/// MalformedReason 对应的文案标识。
///
/// 返回标识而不是现成的字符串，是为了保住 constexpr —— 真正的文字要按用户当前
/// 语言渲染，调用方用 `Text(MalformedReasonMessage(r))` 取。
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

/// MalformedReason 的可读名字（按当前语言）。
[[nodiscard]] inline std::string_view MalformedReasonName(MalformedReason reason) noexcept {
  return Text(MalformedReasonMessage(reason));
}

// =============================================================================
// 编码：host → device
// =============================================================================

/// 把多个以太帧聚合成一次 bulk OUT 传输。
///
/// 对齐处理的细节（这是最容易写错的地方）：
///   RNDIS 规定「除最后一个之外，每个 PACKET_MSG 的 MessageLength 都包含尾部
///   对齐填充；最后一个不含外部填充」。因此本实现在追加**下一个**消息时，才
///   回头把**上一个**消息的 MessageLength 扩大以吞掉中间的填充字节 ——
///   这样最后一个消息的 MessageLength 天然就是 44 + 帧长，完全符合规范。
class PacketMessageWriter {
 public:
  /// @param transfer      输出缓冲（通常是一个 libusb 传输的 buffer）
  /// @param limits        设备协商出的聚合上限
  /// @param endpoint_max_packet  bulk OUT 端点的 wMaxPacketSize，用于规避 ZLP
  struct Limits {
    std::uint32_t max_transfer_bytes = 0;      ///< 设备的 MaxTransferSize。
    std::uint32_t max_messages = 1;            ///< 设备的 MaxPacketsPerMessage。
    std::uint32_t alignment_bytes = 1;         ///< 1 << PacketAlignmentFactor。
  };

  PacketMessageWriter(std::span<std::byte> transfer, const Limits& limits,
                      std::uint32_t endpoint_max_packet) noexcept
      : transfer_(transfer),
        endpoint_max_packet_(endpoint_max_packet),
        max_messages_(limits.max_messages == 0 ? 1 : limits.max_messages),
        alignment_bytes_(limits.alignment_bytes == 0 ? 1 : limits.alignment_bytes) {
    // 有效容量取「缓冲区大小」与「设备 MaxTransferSize」的较小值，
    // 再预留 1 字节给可能需要的 ZLP 规避填充。
    const std::size_t device_limit = limits.max_transfer_bytes == 0
                                         ? transfer.size()
                                         : static_cast<std::size_t>(limits.max_transfer_bytes);
    const std::size_t usable = device_limit < transfer.size() ? device_limit : transfer.size();
    capacity_ = usable > kZlpPadReserve ? usable - kZlpPadReserve : 0;
  }

  /// 尝试追加一帧。返回 false 表示本批次已满，调用方应先提交再重试。
  ///
  /// 帧长必须 >= 14；调用方负责保证（BPF 读上来的帧一定满足）。
  [[nodiscard]] bool TryAppend(std::span<const std::byte> frame) noexcept {
    if (message_count_ >= max_messages_) [[unlikely]] {
      return false;
    }

    const auto frame_length = static_cast<std::uint32_t>(frame.size());

    // 若已有消息，先算出为满足对齐需要插入多少填充。
    const std::size_t aligned_cursor =
        message_count_ == 0 ? cursor_ : AlignUp<std::size_t>(cursor_, alignment_bytes_);
    const std::size_t needed = aligned_cursor + kPacketMsgHeaderBytes + frame_length;
    if (needed > capacity_) {
      return false;
    }

    // 把填充字节归入**上一个**消息的 MessageLength（见类文档注释）。
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
    // DataOffset 是 36 而不是 44 —— 基准点是消息起始 +8。
    StoreLe32(message + kPacketDataOffsetOffset, kPacketInlineDataOffset);
    StoreLe32(message + kPacketDataLengthOffset, frame_length);
    // 其余字段（OOB、per-packet info、VcHandle、Reserved）本项目一律为 0。
    std::memset(message + kPacketOobDataOffsetOffset, 0,
                kPacketMsgHeaderBytes - kPacketOobDataOffsetOffset);

    std::memcpy(message + kPacketMsgHeaderBytes, frame.data(), frame_length);

    last_header_offset_ = cursor_;
    cursor_ += message_length;
    payload_bytes_ += frame_length;
    ++message_count_;
    return true;
  }

  /// 结束本批次，返回应提交给 libusb 的字节数。
  ///
  /// 会在必要时追加 1 个 0x00 字节以规避 ZLP：RNDIS 明确要求 host **不得**发
  /// 零长度包，而当传输长度恰为端点 wMaxPacketSize 的整数倍时，USB 主机控制器
  /// 需要一个短包来标记传输结束。Linux 的 usbnet 就是这么做的 —— 多出的字节在
  /// 所有 MessageLength 之外，设备必须容忍尾部垃圾。
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

  /// 已聚合的以太帧净荷字节数（不含 RNDIS 头与填充），用于统计吞吐。
  [[nodiscard]] std::uint64_t PayloadBytes() const noexcept { return payload_bytes_; }

  [[nodiscard]] bool ZlpPaddingAdded() const noexcept { return zlp_padding_added_; }

  [[nodiscard]] bool Empty() const noexcept { return message_count_ == 0; }

  /// 当前批次还能容纳的最大帧长（0 表示装不下任何帧了）。
  /// 调用方可用它避免「取出一帧却发现放不进去」的回退逻辑。
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
  /// 为 ZLP 规避预留的字节数。
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
