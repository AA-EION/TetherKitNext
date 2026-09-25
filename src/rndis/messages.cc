#include "tetherkitnext/rndis/messages.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"

namespace tetherkitnext::rndis {
namespace {

/// 构造一个带 RNDIS 状态码名字的错误。
///
/// 状态码到名字的映射只存在于 protocol.cc 一处（tk_common 不认识 RNDIS），
/// 因此这里把名字直接拼进 context 串，Error 自身只保留原始数值。
Error MakeStatusError(std::uint32_t status, std::string_view what) {
  const std::string_view name = StatusName(status);
  if (name.empty()) {
    return Error::FromRndisStatus(status, std::string{what});
  }
  return Error::FromRndisStatus(status, Tr(Msg::kRndisStatusSuffix, what, name));
}

/// 校验缓冲区至少有 `needed` 字节。
Status RequireBytes(std::span<const std::byte> buffer, std::size_t needed, std::string_view what) {
  if (buffer.size() < needed) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kRndisBufferTooShort, what, needed, buffer.size())));
  }
  return Ok();
}

/// 校验待写入缓冲区容量。
Status RequireCapacity(std::span<std::byte> buffer, std::size_t needed, std::string_view what) {
  if (buffer.size() < needed) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kRndisEncodeBufferTooSmall, what, needed, buffer.size())));
  }
  return Ok();
}

/// 校验一条完成消息：类型正确、长度自洽、状态成功。
Status ValidateCompletion(std::span<const std::byte> buffer, MessageType expected_type,
                          std::uint32_t minimum_bytes, std::string_view what) {
  TETHERKITNEXT_RETURN_IF_ERROR(RequireBytes(buffer, minimum_bytes, what));

  const std::uint32_t message_type = LoadLe32(buffer.data() + kMessageTypeOffset);
  if (message_type != ToRaw(expected_type)) {
    const std::string_view actual = MessageTypeName(message_type);
    return std::unexpected(Error::Generic(Tr(
        Msg::kRndisUnexpectedMessageType, what,
        actual.empty() ? Text(Msg::kRndisUnknownMessageType) : actual, message_type)));
  }

  const std::uint32_t message_length = LoadLe32(buffer.data() + kMessageLengthOffset);
  if (message_length < minimum_bytes || message_length > buffer.size()) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisInconsistentMessageLength, what,
                                             message_length, buffer.size(), minimum_bytes)));
  }
  return Ok();
}

/// 写入公共头部（Type + Length）。
void WriteHeader(std::span<std::byte> buffer, MessageType type, std::uint32_t message_length) {
  StoreLe32(buffer.data() + kMessageTypeOffset, ToRaw(type));
  StoreLe32(buffer.data() + kMessageLengthOffset, message_length);
}

/// 解析一个「偏移 + 长度」描述的内嵌缓冲区，基准点为消息起始 + 8。
///
/// 返回指向 `buffer` 内部的视图。偏移或长度越界时返回错误。
Result<std::span<const std::byte>> ResolveInlineBuffer(std::span<const std::byte> buffer,
                                                       std::uint32_t message_length,
                                                       std::uint32_t relative_offset,
                                                       std::uint32_t length,
                                                       std::string_view what) {
  if (length == 0) {
    return std::span<const std::byte>{};
  }
  // 绝对偏移 = 8 + 字段值，见 protocol.h 文件头规则 2。
  const std::uint64_t absolute_offset =
      static_cast<std::uint64_t>(kOffsetFieldBase) + relative_offset;
  const std::uint64_t end = absolute_offset + length;
  if (end > message_length) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisInlineBufferOutOfBounds, what,
                                             absolute_offset, relative_offset, length,
                                             message_length)));
  }
  return buffer.subspan(static_cast<std::size_t>(absolute_offset), length);
}

}  // namespace

std::array<char, 18> FormatMac(const MacAddress& mac) noexcept {
  std::array<char, 18> text{};
  std::snprintf(text.data(), text.size(), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
  return text;
}

// =============================================================================
// 通用头部
// =============================================================================

Result<MessageHeader> DecodeMessageHeader(std::span<const std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(
      RequireBytes(buffer, kMessageHeaderBytes, Text(Msg::kRndisWhatMessageHeader)));
  return MessageHeader{
      .message_type = LoadLe32(buffer.data() + kMessageTypeOffset),
      .message_length = LoadLe32(buffer.data() + kMessageLengthOffset),
  };
}

// =============================================================================
// INITIALIZE
// =============================================================================

Result<std::uint32_t> Encode(const InitializeRequest& request, std::span<std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(
      RequireCapacity(buffer, kInitializeMsgBytes, "REMOTE_NDIS_INITIALIZE_MSG"));

  WriteHeader(buffer, MessageType::kInitialize, kInitializeMsgBytes);
  StoreLe32(buffer.data() + kInitializeRequestIdOffset, request.request_id);
  StoreLe32(buffer.data() + kInitializeMajorVersionOffset, request.major_version);
  StoreLe32(buffer.data() + kInitializeMinorVersionOffset, request.minor_version);
  StoreLe32(buffer.data() + kInitializeMaxTransferSizeOffset, request.max_transfer_size);
  return kInitializeMsgBytes;
}

Result<InitializeComplete> DecodeInitializeComplete(std::span<const std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kInitializeComplete,
                                               kInitializeCmpltBytes,
                                               "REMOTE_NDIS_INITIALIZE_CMPLT"));

  const std::byte* base = buffer.data();
  InitializeComplete complete{
      .request_id = LoadLe32(base + kInitializeCmpltRequestIdOffset),
      .status = LoadLe32(base + kInitializeCmpltStatusOffset),
      .major_version = LoadLe32(base + kInitializeCmpltMajorVersionOffset),
      .minor_version = LoadLe32(base + kInitializeCmpltMinorVersionOffset),
      .device_flags = LoadLe32(base + kInitializeCmpltDeviceFlagsOffset),
      .medium = LoadLe32(base + kInitializeCmpltMediumOffset),
      .max_packets_per_message = LoadLe32(base + kInitializeCmpltMaxPacketsPerMessageOffset),
      .max_transfer_size = LoadLe32(base + kInitializeCmpltMaxTransferSizeOffset),
      .packet_alignment_factor = LoadLe32(base + kInitializeCmpltPacketAlignmentFactorOffset),
      .af_list_offset = LoadLe32(base + kInitializeCmpltAfListOffsetOffset),
      .af_list_size = LoadLe32(base + kInitializeCmpltAfListSizeOffset),
  };

  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    return std::unexpected(MakeStatusError(complete.status, Tr(Msg::kRndisInitRejected)));
  }
  return complete;
}

Result<NegotiatedParameters> Negotiate(const InitializeComplete& complete,
                                       std::uint32_t requested_mtu,
                                       std::uint32_t host_transfer_size_limit) {
  // ---- 介质必须是以太网 ----
  if (complete.medium != ToRaw(Medium::kEthernet)) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisUnsupportedMedium, complete.medium)));
  }

  // ---- 面向连接设备不支持 ----
  const bool connectionless =
      (complete.device_flags & static_cast<std::uint32_t>(DeviceFlags::kConnectionless)) != 0;
  const bool connection_oriented =
      (complete.device_flags & static_cast<std::uint32_t>(DeviceFlags::kConnectionOriented)) != 0;
  if (connection_oriented && !connectionless) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisConnectionOriented)));
  }
  if (!connectionless) {
    // 有设备两个位都不置。宽容处理：按无连接继续，只告警。
    TETHERKITNEXT_WARN_TR(Msg::kRndisNoConnectionlessFlag, complete.device_flags);
  }

  // ---- 版本 ----
  if (complete.major_version != kMajorVersion) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kRndisUnsupportedMajorVersion, complete.major_version, kMajorVersion)));
  }

  NegotiatedParameters params;
  params.connectionless = true;

  // ---- MaxPacketsPerMessage：0 当 1 ----
  // 主线 Linux gadget 报 1；打了高通上行聚合补丁的 Android 内核报 3 或更多。
  params.max_packets_per_message = std::max<std::uint32_t>(1, complete.max_packets_per_message);

  // ---- PacketAlignmentFactor：钳到 7 ----
  // 超过 7 属于协议违规。不钳位的话 1u << factor 会溢出或算出荒谬的填充长度。
  std::uint32_t alignment_factor = complete.packet_alignment_factor;
  if (alignment_factor > kMaxPacketAlignmentFactor) {
    TETHERKITNEXT_WARN_TR(Msg::kRndisAlignmentFactorClamped, alignment_factor,
                      kMaxPacketAlignmentFactor);
    alignment_factor = kMaxPacketAlignmentFactor;
  }
  params.tx_alignment_bytes = 1U << alignment_factor;

  // ---- MaxTransferSize：双向钳位 ----
  std::uint32_t device_limit = complete.max_transfer_size;
  if (device_limit <= kHardHeaderBytes) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kRndisMaxTransferTooSmall, device_limit, kHardHeaderBytes)));
  }

  // 上钳：某些 WinCE / Windows Mobile 设备宣称 8KB 或 16KB 的巨帧上限，
  // 对这种链路速率毫无意义，只会让我们分配巨大的传输缓冲。不盲从设备。
  if (device_limit > host_transfer_size_limit) {
    TETHERKITNEXT_INFO_TR(Msg::kRndisMaxTransferClampedToHost, device_limit,
                      host_transfer_size_limit);
    device_limit = host_transfer_size_limit;
  }
  params.device_max_transfer_size = device_limit;

  // 下钳：若设备装不下一个按 requested_mtu 计算的满帧，就反推出可行的 MTU。
  // 实测案例：HTC Diamond 报 1536，比 hard_mtu(1558) 小，MTU 需降到 1478。
  params.mtu = requested_mtu;
  const std::uint32_t hard_mtu = HardMtuFor(requested_mtu);
  if (device_limit < hard_mtu) {
    params.mtu = device_limit - kHardHeaderBytes;
    TETHERKITNEXT_WARN_TR(Msg::kRndisMtuLoweredForTransferSize, device_limit, hard_mtu, requested_mtu,
                      params.mtu);
  }

  return params;
}

// =============================================================================
// HALT
// =============================================================================

Result<std::uint32_t> EncodeHalt(std::uint32_t request_id, std::span<std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(RequireCapacity(buffer, kHaltMsgBytes, "REMOTE_NDIS_HALT_MSG"));
  WriteHeader(buffer, MessageType::kHalt, kHaltMsgBytes);
  StoreLe32(buffer.data() + kHaltRequestIdOffset, request_id);
  return kHaltMsgBytes;
}

// =============================================================================
// QUERY / SET
// =============================================================================

Result<std::uint32_t> Encode(const QueryRequest& request, std::span<std::byte> buffer) {
  // 未文档化但必须遵守的规则（见 QueryRequest::expected_response_bytes 注释）：
  //   变长 OID → InformationBufferLength 必须为 0；
  //   定长 OID → 必须 ≥ 期望响应长度，且消息尾部要真的有那么多字节。
  const bool variable_length = IsVariableLengthOid(request.oid);
  const std::uint32_t info_length = variable_length ? 0 : request.expected_response_bytes;

  const std::uint32_t message_length = kQuerySetHeaderBytes + info_length;
  TETHERKITNEXT_RETURN_IF_ERROR(RequireCapacity(buffer, message_length, "REMOTE_NDIS_QUERY_MSG"));

  WriteHeader(buffer, MessageType::kQuery, message_length);
  StoreLe32(buffer.data() + kQuerySetRequestIdOffset, request.request_id);
  StoreLe32(buffer.data() + kQuerySetOidOffset, ToRaw(request.oid));
  StoreLe32(buffer.data() + kQuerySetInfoBufferLengthOffset, info_length);
  // 长度为 0 时偏移也必须为 0，否则某些设备会拒绝。
  StoreLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset,
            info_length == 0 ? 0 : kQuerySetInlineInfoOffset);
  StoreLe32(buffer.data() + kQuerySetDeviceVcHandleOffset, 0);

  // 尾部填零，作为设备写回结果的占位区。
  if (info_length != 0) {
    std::memset(buffer.data() + kQuerySetHeaderBytes, 0, info_length);
  }
  return message_length;
}

Result<QueryComplete> DecodeQueryComplete(std::span<const std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kQueryComplete,
                                               kQueryCmpltHeaderBytes,
                                               "REMOTE_NDIS_QUERY_CMPLT"));

  const std::byte* base = buffer.data();
  const std::uint32_t message_length = LoadLe32(base + kMessageLengthOffset);
  const std::uint32_t status = LoadLe32(base + kQueryCmpltStatusOffset);
  const std::uint32_t info_length = LoadLe32(base + kQueryCmpltInfoBufferLengthOffset);
  const std::uint32_t info_offset = LoadLe32(base + kQueryCmpltInfoBufferOffsetOffset);

  QueryComplete complete{
      .request_id = LoadLe32(base + kQueryCmpltRequestIdOffset),
      .status = status,
      .information = {},
  };

  if (status != ToRaw(StatusCode::kSuccess)) {
    // 失败时不解析信息缓冲区 —— 设备可能没填。把状态原样返回给调用方判断，
    // 因为「某个可选 OID 不支持」是正常情况，不该一律当致命错误。
    return complete;
  }

  TETHERKITNEXT_ASSIGN_OR_RETURN(
      complete.information,
      ResolveInlineBuffer(buffer, message_length, info_offset, info_length,
                          Text(Msg::kRndisWhatQueryCmpltInfoBuffer)));
  return complete;
}

Result<std::uint32_t> Encode(const SetRequest& request, std::span<std::byte> buffer) {
  const auto info_length = static_cast<std::uint32_t>(request.information.size());
  const std::uint32_t message_length = kQuerySetHeaderBytes + info_length;
  TETHERKITNEXT_RETURN_IF_ERROR(RequireCapacity(buffer, message_length, "REMOTE_NDIS_SET_MSG"));

  WriteHeader(buffer, MessageType::kSet, message_length);
  StoreLe32(buffer.data() + kQuerySetRequestIdOffset, request.request_id);
  StoreLe32(buffer.data() + kQuerySetOidOffset, ToRaw(request.oid));
  StoreLe32(buffer.data() + kQuerySetInfoBufferLengthOffset, info_length);
  StoreLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset,
            info_length == 0 ? 0 : kQuerySetInlineInfoOffset);
  StoreLe32(buffer.data() + kQuerySetDeviceVcHandleOffset, 0);

  if (info_length != 0) {
    std::memcpy(buffer.data() + kQuerySetHeaderBytes, request.information.data(), info_length);
  }
  return message_length;
}

Result<std::uint32_t> EncodeSetUint32(std::uint32_t request_id, Oid oid, std::uint32_t value,
                                      std::span<std::byte> buffer) {
  std::array<std::byte, 4> payload{};
  StoreLe32(payload.data(), value);
  return Encode(SetRequest{.request_id = request_id, .oid = oid, .information = payload}, buffer);
}

Result<SetComplete> DecodeSetComplete(std::span<const std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(
      ValidateCompletion(buffer, MessageType::kSetComplete, kSetCmpltBytes,
                         "REMOTE_NDIS_SET_CMPLT"));
  return SetComplete{
      .request_id = LoadLe32(buffer.data() + kSetCmpltRequestIdOffset),
      .status = LoadLe32(buffer.data() + kSetCmpltStatusOffset),
  };
}

// =============================================================================
// RESET
// =============================================================================

Result<std::uint32_t> EncodeReset(std::span<std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(RequireCapacity(buffer, kResetMsgBytes, "REMOTE_NDIS_RESET_MSG"));
  WriteHeader(buffer, MessageType::kReset, kResetMsgBytes);
  // offset 8 是 Reserved，不是 RequestId —— 必须写 0。
  StoreLe32(buffer.data() + kResetReservedOffset, 0);
  return kResetMsgBytes;
}

Result<ResetComplete> DecodeResetComplete(std::span<const std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kResetComplete,
                                               kResetCmpltBytes, "REMOTE_NDIS_RESET_CMPLT"));
  // 注意偏移：RESET_CMPLT 没有 RequestId，Status 在 offset 8 而非 12。
  return ResetComplete{
      .status = LoadLe32(buffer.data() + kResetCmpltStatusOffset),
      .addressing_reset = LoadLe32(buffer.data() + kResetCmpltAddressingResetOffset) != 0,
  };
}

// =============================================================================
// KEEPALIVE
// =============================================================================

Result<std::uint32_t> EncodeKeepAlive(std::uint32_t request_id, std::span<std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(
      RequireCapacity(buffer, kKeepAliveMsgBytes, "REMOTE_NDIS_KEEPALIVE_MSG"));
  WriteHeader(buffer, MessageType::kKeepAlive, kKeepAliveMsgBytes);
  StoreLe32(buffer.data() + kKeepAliveRequestIdOffset, request_id);
  return kKeepAliveMsgBytes;
}

Result<KeepAliveComplete> DecodeKeepAliveComplete(std::span<const std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(ValidateCompletion(buffer, MessageType::kKeepAliveComplete,
                                               kKeepAliveCmpltBytes,
                                               "REMOTE_NDIS_KEEPALIVE_CMPLT"));
  return KeepAliveComplete{
      .request_id = LoadLe32(buffer.data() + kKeepAliveCmpltRequestIdOffset),
      .status = LoadLe32(buffer.data() + kKeepAliveCmpltStatusOffset),
  };
}

Result<std::uint32_t> EncodeKeepAliveComplete(std::uint32_t request_id, std::uint32_t status,
                                              std::span<std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(
      RequireCapacity(buffer, kKeepAliveCmpltBytes, "REMOTE_NDIS_KEEPALIVE_CMPLT"));
  WriteHeader(buffer, MessageType::kKeepAliveComplete, kKeepAliveCmpltBytes);
  StoreLe32(buffer.data() + kKeepAliveCmpltRequestIdOffset, request_id);
  StoreLe32(buffer.data() + kKeepAliveCmpltStatusOffset, status);
  return kKeepAliveCmpltBytes;
}

// =============================================================================
// INDICATE_STATUS
// =============================================================================

Result<IndicateStatus> DecodeIndicateStatus(std::span<const std::byte> buffer) {
  TETHERKITNEXT_RETURN_IF_ERROR(RequireBytes(buffer, kIndicateStatusHeaderBytes,
                                         "REMOTE_NDIS_INDICATE_STATUS_MSG"));

  const std::byte* base = buffer.data();
  const std::uint32_t message_type = LoadLe32(base + kMessageTypeOffset);
  if (message_type != ToRaw(MessageType::kIndicateStatus)) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisExpectedIndicateStatus, message_type)));
  }

  const std::uint32_t message_length =
      std::min<std::uint32_t>(LoadLe32(base + kMessageLengthOffset),
                              static_cast<std::uint32_t>(buffer.size()));

  IndicateStatus indication{
      .status = LoadLe32(base + kIndicateStatusStatusOffset),
      .status_buffer = {},
  };

  const std::uint32_t buffer_length = LoadLe32(base + kIndicateStatusBufferLengthOffset);
  if (buffer_length == 0) {
    // 绝大多数情况：MEDIA_CONNECT / MEDIA_DISCONNECT 不带负载。
    return indication;
  }

  // StatusBufferOffset 的基准点在规范里是矛盾的，两种解释都试一遍。
  // 详见 messages.h 中 DecodeIndicateStatus 的文档注释。
  const std::uint32_t relative_offset = LoadLe32(base + kIndicateStatusBufferOffsetOffset);
  const std::uint64_t candidates[] = {
      static_cast<std::uint64_t>(kOffsetFieldBase) + relative_offset,  // 与 QUERY/SET 一致
      relative_offset,                                                 // 按 MS 文档字面
  };
  for (const std::uint64_t offset : candidates) {
    if (offset + buffer_length <= message_length) {
      indication.status_buffer =
          buffer.subspan(static_cast<std::size_t>(offset), buffer_length);
      break;
    }
  }

  if (indication.status_buffer.empty()) {
    // 两种解释都越界：丢掉可选负载，但**仍然成功返回** —— status 本身有用
    // （比如它可能就是 MEDIA_DISCONNECT），不能因为解析不了一个可选字段
    // 就把链路判死。
    TETHERKITNEXT_WARN_TR(Msg::kRndisIndicateStatusBufferOutOfBounds, relative_offset, buffer_length,
                      message_length);
    return indication;
  }

  // 恰好 8 字节时按 RNDIS_Diagnostic_Info 解析 —— 设备用它报告我们发过去的
  // 消息哪里不合法（DiagStatus + ErrorOffset），对排障极有价值。
  if (indication.status_buffer.size() == kDiagnosticInfoBytes) {
    indication.has_diagnostic_info = true;
    indication.diagnostic_status =
        LoadLe32(indication.status_buffer.data() + kDiagnosticInfoDiagStatusOffset);
    indication.diagnostic_error_offset =
        LoadLe32(indication.status_buffer.data() + kDiagnosticInfoErrorOffsetOffset);
  }
  return indication;
}

// =============================================================================
// OID 负载解析
// =============================================================================

Result<std::uint32_t> ParseUint32(std::span<const std::byte> information) {
  TETHERKITNEXT_RETURN_IF_ERROR(RequireBytes(information, 4, Text(Msg::kRndisWhatOidUint32)));
  return LoadLe32(information.data());
}

Result<std::uint64_t> ParseCounter(std::span<const std::byte> information) {
  // 统计类 OID 可能返回 4 或 8 字节，两者都必须接受。
  if (information.size() >= 8) {
    return LoadLe64(information.data());
  }
  if (information.size() >= 4) {
    return static_cast<std::uint64_t>(LoadLe32(information.data()));
  }
  return std::unexpected(Error::Generic(Tr(Msg::kRndisOidCounterBadLength, information.size())));
}

Result<MacAddress> ParseMac(std::span<const std::byte> information) {
  TETHERKITNEXT_RETURN_IF_ERROR(
      RequireBytes(information, sizeof(MacAddress), Text(Msg::kRndisWhatOidMac)));
  MacAddress mac{};
  std::memcpy(mac.data(), information.data(), mac.size());
  return mac;
}

}  // namespace tetherkitnext::rndis
