#include "tetherkitnext/rndis/state_machine.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"

namespace tetherkitnext::rndis {
namespace {

/// QUERY 定长 OID 时给的占位缓冲大小。
///
/// 对返回定长结果的 OID，InformationBufferLength 必须 >= 期望响应长度，否则微软
/// ActiveSync 实现会回 RNDIS_STATUS_INVALID_LENGTH。Linux 查 MAC 时给的是 48，
/// 这里沿用同一个「宽松但足够」的值作为多数定长 OID 的默认占位。
constexpr std::uint32_t kFixedOidPlaceholderBytes = 48;

/// 从可能不带 NUL 结尾的 ASCII 缓冲里取出字符串。
[[nodiscard]] std::string ToPrintableString(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    const auto character = static_cast<char>(byte);
    if (character == '\0') {
      break;  // 有些设备确实带 NUL，遇到就停
    }
    // 过滤掉不可打印字符，避免把控制字符打进日志。
    text.push_back(character >= 0x20 && character < 0x7F ? character : '?');
  }
  return text;
}

}  // namespace

std::string_view StateName(State state) noexcept {
  switch (state) {
    case State::kUninitialized:
      return Text(Msg::kRndisStateUninitialized);
    case State::kInitializing:
      return Text(Msg::kRndisStateInitializing);
    case State::kInitialized:
      return Text(Msg::kRndisStateInitialized);
    case State::kDataInitialized:
      return Text(Msg::kRndisStateDataInitialized);
    case State::kHalting:
      return Text(Msg::kRndisStateHalting);
  }
  return Text(Msg::kRndisStateUnknown);
}

StateMachine::StateMachine(ControlChannel& channel, StateMachineObserver& observer,
                           const StateMachineConfig& config)
    : channel_(&channel), observer_(&observer), config_(config) {
  request_buffer_.resize(kControlBufferBytes);
  last_device_activity_ = MonotonicNanos();
  next_keepalive_deadline_ =
      last_device_activity_ +
      static_cast<Nanos>(config_.keepalive_interval_millis) * kNanosPerMilli;
}

std::uint32_t StateMachine::NextRequestId() noexcept {
  // 跳过 0：某些设备把 RequestId == 0 当作无效值（FreeBSD 的实现干脆全填 0，
  // 说明设备普遍不校验，但保守起见还是避开）。
  const std::uint32_t id = next_request_id_++;
  if (next_request_id_ == 0) {
    next_request_id_ = 1;
  }
  return id;
}

void StateMachine::TransitionTo(State next) {
  if (state_ == next) {
    return;
  }
  const State previous = state_;
  state_ = next;
  TETHERKITNEXT_INFO_TR(Msg::kRndisStateTransition, StateName(previous), StateName(next));
  observer_->OnStateChanged(previous, next);
}

// =============================================================================
// 控制通道往返
// =============================================================================

Result<bool> StateMachine::PumpOnce(std::optional<MessageType> expected_reply,
                                   std::span<const std::byte>& out_response) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const std::span<const std::byte> message,
                             channel_->ReceiveMessage());

  if (message.empty()) {
    // 规范：设备尚无有效响应时返回 1 字节 0x00 而非 STALL。不是错误。
    return false;
  }
  MarkDeviceActivity();

  TETHERKITNEXT_ASSIGN_OR_RETURN(const MessageHeader header, DecodeMessageHeader(message));
  const std::string_view name = MessageTypeName(header.message_type);
  TETHERKITNEXT_DEBUG_TR(Msg::kRndisControlMessageReceived,
                     name.empty() ? Text(Msg::kRndisUnknownMessage) : name, header.message_type,
                     header.message_length);

  // ---- 设备主动推送的消息：分派掉，继续等我们要的那条 ----
  if (header.message_type == ToRaw(MessageType::kIndicateStatus)) {
    HandleIndicateStatus(message);
    return false;
  }
  if (header.message_type == ToRaw(MessageType::kKeepAlive)) {
    // 设备发起的保活。**必须回复**，否则设备可能判定主机已死而断开。
    TETHERKITNEXT_RETURN_IF_ERROR(HandleDeviceKeepAlive(message));
    return false;
  }

  // ---- 面向连接设备的消息：明确报不支持，而不是含糊的「未知消息」----
  if (IsCondis(header.message_type)) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisCondisMessageReceived,
                                             name.empty() ? Text(Msg::kRndisUnknown) : name)));
  }

  // ---- 是我们等的那条吗 ----
  if (expected_reply.has_value() && header.message_type == ToRaw(*expected_reply)) {
    out_response = message;
    return true;
  }

  // 不是我们等的，也不是已知的推送消息。可能是上一次超时请求的迟到响应 ——
  // 丢弃并继续（不能当致命错误，否则一次超时就会把链路判死）。
  TETHERKITNEXT_WARN_TR(Msg::kRndisDiscardUnexpectedMessage,
                    name.empty() ? Text(Msg::kRndisUnknown) : name, header.message_type);
  return false;
}

Result<StateMachine::Exchange> StateMachine::Transact(std::span<const std::byte> request,
                                                      MessageType expected_reply) {
  TETHERKITNEXT_RETURN_IF_ERROR(channel_->SendMessage(request));

  // 先等中断端点的 RESPONSE_AVAILABLE 通知，再去取响应。
  //
  // 两类设备行为都要兼容：
  //   * 规范做法是等通知；
  //   * 但也存在「必须先被中断端点读过一次才会在控制端点上作答」的设备；
  //   * 而 Linux 干脆完全忽略中断端点，直接轮询控制端点。
  // 所以：有中断端点就先等一次通知（超时也不算错），然后无论如何都去轮询。
  // 同样不能为 0（timeout=0 在 darwin 上是无限等待）。注意 response_poll_interval
  // 可以被配成 0（测试里就是），所以这里必须用 max 兜底。
  const std::uint32_t notification_timeout =
      std::max(kProbeOnlyTimeoutMillis,
               std::min(config_.control_timeout_millis, config_.response_poll_interval_millis * 4));
  const NotificationResult notification =
      channel_->WaitForNotification(notification_timeout);
  if (notification == NotificationResult::kResponseAvailable) {
    TETHERKITNEXT_TRACE_TR(Msg::kRndisNotificationReceived);
  }

  std::span<const std::byte> response;
  for (std::uint32_t attempt = 0; attempt < config_.response_poll_attempts; ++attempt) {
    TETHERKITNEXT_ASSIGN_OR_RETURN(const bool matched, PumpOnce(expected_reply, response));
    if (matched) {
      return Exchange{.response = response};
    }
    // 还没准备好。睡一小会儿再试 —— 这里必须睡，否则会把控制端点打成忙轮询，
    // 而 Apple Silicon 上每次控制传输本身就是毫秒级开销。
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.response_poll_interval_millis));
  }

  const std::string_view name = MessageTypeName(ToRaw(expected_reply));
  return std::unexpected(Error::Generic(
      Tr(Msg::kRndisWaitReplyTimeout, name.empty() ? Text(Msg::kRndisResponse) : name,
         config_.response_poll_attempts, config_.response_poll_interval_millis)));
}

// =============================================================================
// 设备推送消息的处理
// =============================================================================

void StateMachine::HandleIndicateStatus(std::span<const std::byte> message) {
  const auto indication = DecodeIndicateStatus(message);
  if (!indication) {
    // 解析失败不该让链路死掉 —— 这是个纯通报消息。
    TETHERKITNEXT_WARN_TR(Msg::kRndisIndicateStatusParseFailed, indication.error().ToString());
    return;
  }

  const std::string_view status_name = StatusName(indication->status);
  if (indication->has_diagnostic_info) {
    // 设备用 Rndis_Diagnostic_Info 告诉我们「你发过来的消息第 N 字节不合法」，
    // 对排查我们自己的编码 bug 极有价值，所以单独打出来。
    TETHERKITNEXT_WARN_TR(Msg::kRndisDeviceIndicatedWithDiagnostics,
                      status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name,
                      indication->status, indication->diagnostic_status,
                      indication->diagnostic_error_offset);
  } else {
    TETHERKITNEXT_INFO_TR(Msg::kRndisDeviceIndicated,
                      status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name,
                      indication->status);
  }

  switch (static_cast<StatusCode>(indication->status)) {
    case StatusCode::kMediaConnect:
      if (!link_connected_) {
        link_connected_ = true;
        info_.media_state = MediaState::kConnected;
        observer_->OnLinkStateChanged(true);
      }
      break;

    case StatusCode::kMediaDisconnect:
      if (link_connected_) {
        link_connected_ = false;
        info_.media_state = MediaState::kDisconnected;
        observer_->OnLinkStateChanged(false);
      }
      break;

    default:
      // 其它状态只记日志。特别是 kInvalidData —— 它说明我们发的消息有问题，
      // 但不该据此断开链路（诊断信息已经打出来了）。
      break;
  }
}

Status StateMachine::HandleDeviceKeepAlive(std::span<const std::byte> message) {
  // 设备发起的 KEEPALIVE_MSG 的 RequestId 在 offset 8，与主机发起时同布局。
  if (message.size() < kKeepAliveMsgBytes) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisKeepAliveMsgTooShort)));
  }
  const std::uint32_t request_id = LoadLe32(message.data() + kKeepAliveRequestIdOffset);
  TETHERKITNEXT_DEBUG_TR(Msg::kRndisDeviceKeepAlive, request_id);

  TETHERKITNEXT_ASSIGN_OR_RETURN(
      const std::uint32_t written,
      EncodeKeepAliveComplete(request_id, ToRaw(StatusCode::kSuccess), request_buffer_));
  return channel_->SendMessage(std::span<const std::byte>{request_buffer_.data(), written});
}

// =============================================================================
// OID 读写
// =============================================================================

Result<std::span<const std::byte>> StateMachine::QueryOid(Oid oid, std::uint32_t expected_bytes,
                                                          bool fatal) {
  const std::string_view name = OidName(ToRaw(oid));
  TETHERKITNEXT_ASSIGN_OR_RETURN(
      const std::uint32_t written,
      Encode(QueryRequest{.request_id = NextRequestId(),
                          .oid = oid,
                          .expected_response_bytes = expected_bytes},
             request_buffer_));

  auto exchange = Transact(std::span<const std::byte>{request_buffer_.data(), written},
                           MessageType::kQueryComplete);
  if (!exchange) {
    if (!fatal) {
      TETHERKITNEXT_DEBUG_TR(Msg::kRndisOptionalOidQueryFailed, name, exchange.error().ToString());
      return std::span<const std::byte>{};
    }
    return std::unexpected(
        std::move(exchange).error().WithContext(Tr(Msg::kRndisOidQueryFailed, name)));
  }

  TETHERKITNEXT_ASSIGN_OR_RETURN(const QueryComplete complete,
                             DecodeQueryComplete(exchange->response));

  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    const std::string_view status_name = StatusName(complete.status);
    if (!fatal) {
      // 可选 OID 返回 NOT_SUPPORTED 是完全正常的（例如 OID_GEN_PHYSICAL_MEDIUM）。
      TETHERKITNEXT_DEBUG_TR(Msg::kRndisOptionalOidUnsupported, name,
                         status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name);
      return std::span<const std::byte>{};
    }
    return std::unexpected(Error::FromRndisStatus(
        complete.status,
        Tr(Msg::kRndisOidQueryRejected, name,
           status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name)));
  }
  return complete.information;
}

Status StateMachine::SetOidUint32(Oid oid, std::uint32_t value) {
  const std::string_view name = OidName(ToRaw(oid));
  TETHERKITNEXT_ASSIGN_OR_RETURN(
      const std::uint32_t written,
      EncodeSetUint32(NextRequestId(), oid, value, request_buffer_));

  TETHERKITNEXT_ASSIGN_OR_RETURN(
      const Exchange exchange,
      Transact(std::span<const std::byte>{request_buffer_.data(), written},
               MessageType::kSetComplete));

  TETHERKITNEXT_ASSIGN_OR_RETURN(const SetComplete complete, DecodeSetComplete(exchange.response));
  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    const std::string_view status_name = StatusName(complete.status);
    return std::unexpected(Error::FromRndisStatus(
        complete.status,
        Tr(Msg::kRndisOidSetRejected, name, value,
           status_name.empty() ? Text(Msg::kRndisUnknownStatus) : status_name)));
  }
  TETHERKITNEXT_DEBUG_TR(Msg::kRndisOidSet, name, value);
  return Ok();
}

// =============================================================================
// 启动序列
// =============================================================================

Status StateMachine::CollectDeviceInfo() {
  // ---- 物理介质：**可选 OID**，失败不致命 ----
  // Linux 查它时 in_len 传 4，失败就当 UNSPECIFIED 继续。
  if (const auto medium = QueryOid(Oid::kGenPhysicalMedium, 4, /*fatal=*/false);
      medium && !medium->empty()) {
    if (const auto value = ParseUint32(*medium)) {
      info_.physical_medium = static_cast<PhysicalMedium>(*value);
    }
  }

  // ---- 永久 MAC：**致命** ----
  // 这是主机侧 feth 要采用的地址，拿不到就没法正确搭建网卡。
  {
    TETHERKITNEXT_ASSIGN_OR_RETURN(
        const std::span<const std::byte> payload,
        QueryOid(Oid::kEthernetPermanentAddress, kFixedOidPlaceholderBytes, /*fatal=*/true));
    TETHERKITNEXT_ASSIGN_OR_RETURN(info_.permanent_address, ParseMac(payload));
    info_.has_permanent_address = true;
    TETHERKITNEXT_INFO_TR(Msg::kRndisPermanentMac, FormatMac(info_.permanent_address).data());
  }

  // ---- 当前 MAC：不致命（多数设备与永久 MAC 相同）----
  if (const auto payload =
          QueryOid(Oid::kEthernetCurrentAddress, kFixedOidPlaceholderBytes, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto mac = ParseMac(*payload)) {
      info_.current_address = *mac;
      info_.has_current_address = true;
    }
  }
  if (!info_.has_current_address) {
    info_.current_address = info_.permanent_address;
    info_.has_current_address = true;
  }

  // ---- 最大帧长：不致命（协商结果已经给了 MTU）----
  if (const auto payload =
          QueryOid(Oid::kGenMaximumFrameSize, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.maximum_frame_size = *value;
      // 设备说它最大只收 N 字节净荷，而我们协商出的 MTU 更大 —— 听设备的。
      if (*value != 0 && *value < parameters_.mtu) {
        TETHERKITNEXT_WARN_TR(Msg::kRndisMtuLoweredToDeviceFrameSize, *value, parameters_.mtu);
        parameters_.mtu = *value;
      }
    }
  }

  // ---- 链路速率：不致命，仅用于日志 ----
  if (const auto payload = QueryOid(Oid::kGenLinkSpeed, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.link_speed_100bps = *value;
    }
  }

  // ---- 媒体连接状态：不致命。**注意 0 表示已连接** ----
  if (const auto payload = QueryOid(Oid::kGenMediaConnectStatus, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.media_state = static_cast<MediaState>(*value);
      link_connected_ = info_.media_state == MediaState::kConnected;
    }
  } else {
    // 查不到就假定已连接 —— 设备既然在跑 RNDIS，链路大概率是通的。
    link_connected_ = true;
  }

  // ---- 厂商信息：纯诊断用 ----
  if (const auto payload = QueryOid(Oid::kGenVendorId, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.vendor_id = *value;
    }
  }
  // 厂商描述是**变长** OID，expected_bytes 必须传 0。
  if (const auto payload = QueryOid(Oid::kGenVendorDescription, 0, /*fatal=*/false);
      payload && !payload->empty()) {
    info_.vendor_description = ToPrintableString(*payload);
  }

  TETHERKITNEXT_INFO_TR(Msg::kRndisDeviceInfo, FormatMac(info_.permanent_address).data(),
                    info_.LinkSpeedMbps(), info_.maximum_frame_size,
                    static_cast<std::uint32_t>(info_.physical_medium), info_.vendor_id,
                    info_.vendor_description);

  return Ok();
}

Status StateMachine::Start() {
  if (state_ != State::kUninitialized) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisStartWrongState, StateName(state_))));
  }

  TransitionTo(State::kInitializing);

  // ---- 第 1 步：INITIALIZE ----
  {
    TETHERKITNEXT_ASSIGN_OR_RETURN(
        const std::uint32_t written,
        Encode(InitializeRequest{.request_id = NextRequestId(),
                                 .max_transfer_size = config_.host_max_transfer_size},
               request_buffer_));

    auto exchange = Transact(std::span<const std::byte>{request_buffer_.data(), written},
                              MessageType::kInitializeComplete);
    if (!exchange) {
      TransitionTo(State::kUninitialized);
      return std::unexpected(std::move(exchange).error().WithContext(Tr(Msg::kRndisInitFailed)));
    }

    auto complete = DecodeInitializeComplete(exchange->response);
    if (!complete) {
      SendHalt();
      TransitionTo(State::kUninitialized);
      return std::unexpected(std::move(complete).error());
    }

    auto negotiated = Negotiate(*complete, config_.requested_mtu, config_.host_max_transfer_size);
    if (!negotiated) {
      SendHalt();
      TransitionTo(State::kUninitialized);
      return std::unexpected(std::move(negotiated).error());
    }
    parameters_ = *negotiated;

    // 设备汇报的聚合包数只是它的**宣称**，调用方可以不信任它（见配置项注释）。
    if (config_.max_tx_packets_per_message != 0 &&
        parameters_.max_packets_per_message > config_.max_tx_packets_per_message) {
      TETHERKITNEXT_INFO_TR(Msg::kRndisMaxPacketsClamped, parameters_.max_packets_per_message,
                        config_.max_tx_packets_per_message);
      parameters_.max_packets_per_message = config_.max_tx_packets_per_message;
    }

    TETHERKITNEXT_INFO_TR(Msg::kRndisNegotiated, complete->major_version, complete->minor_version,
                      parameters_.mtu, parameters_.device_max_transfer_size,
                      parameters_.max_packets_per_message, parameters_.tx_alignment_bytes);
  }

  TransitionTo(State::kInitialized);

  // ---- 第 2~4 步：采集设备信息 ----
  if (const auto status = CollectDeviceInfo(); !status) {
    SendHalt();
    TransitionTo(State::kUninitialized);
    return status;
  }

  // ---- 第 5 步：设置包过滤 → 设备进入 data-initialized，数据开始流动 ----
  if (const auto status = SetOidUint32(Oid::kGenCurrentPacketFilter, config_.packet_filter);
      !status) {
    SendHalt();
    TransitionTo(State::kUninitialized);
    return std::unexpected(Error{status.error()}.WithContext(Tr(Msg::kRndisPacketFilterFailed)));
  }
  active_packet_filter_ = config_.packet_filter;

  TransitionTo(State::kDataInitialized);
  observer_->OnNegotiated(parameters_, info_);
  observer_->OnLinkStateChanged(link_connected_);

  MarkDeviceActivity();
  next_keepalive_deadline_ =
      MonotonicNanos() +
      static_cast<Nanos>(config_.keepalive_interval_millis) * kNanosPerMilli;
  return Ok();
}

// =============================================================================
// 周期性工作
// =============================================================================

std::uint32_t StateMachine::MillisUntilNextPoll() const noexcept {
  const Nanos now = MonotonicNanos();
  if (now >= next_keepalive_deadline_) {
    return 0;
  }
  return static_cast<std::uint32_t>((next_keepalive_deadline_ - now) / kNanosPerMilli);
}

Status StateMachine::Poll() {
  if (state_ != State::kDataInitialized && state_ != State::kInitialized) {
    return Ok();  // 未就绪或正在停机，无事可做
  }

  // ---- 先排空设备主动推送的消息 ----
  //
  // 不排空的话它们会堆在设备的响应队列里，把我们后续请求的响应挤到后面，
  // 导致每次 Transact 都要多轮几次才拿到想要的那条。
  //
  // 只在有中断通知时才去读控制端点 —— 否则每次 Poll 都要发一次
  // GET_ENCAPSULATED_RESPONSE，而 Apple Silicon 上这是毫秒级开销。
  //
  // ⚠️ 这里**绝不能传 0**：libusb 在 darwin 上 timeout=0 表示无限等待，
  // 会把控制线程永久卡死。用 kProbeOnlyTimeoutMillis（1 ms）——
  // Poll 本身每几百毫秒才跑一次，1 ms 的阻塞可以忽略。
  if (channel_->WaitForNotification(kProbeOnlyTimeoutMillis) ==
      NotificationResult::kResponseAvailable) {
    std::span<const std::byte> ignored;
    // 最多排 8 条，避免设备疯狂推送时卡在这里。
    for (int i = 0; i < 8; ++i) {
      const auto pumped = PumpOnce(std::nullopt, ignored);
      if (!pumped) {
        return std::unexpected(
            Error{pumped.error()}.WithContext(Tr(Msg::kRndisDrainControlFailed)));
      }
      // PumpOnce 在没有更多消息时返回 false 且不报错，无法区分「空了」和
      // 「处理了一条推送」。用通知状态再判一次。
      if (channel_->WaitForNotification(kProbeOnlyTimeoutMillis) !=
          NotificationResult::kResponseAvailable) {
        break;
      }
    }
  }

  // ---- 保活 ----
  //
  // 语义是「距上次从设备收到任何消息已超过保活周期」才发，而不是无条件定时发。
  // 通道活跃时无谓的保活往返纯属浪费（尤其在 Apple Silicon 上）。
  const Nanos now = MonotonicNanos();
  if (now < next_keepalive_deadline_) {
    return Ok();
  }

  const Nanos idle_nanos = now - last_device_activity_;
  const Nanos interval_nanos =
      static_cast<Nanos>(config_.keepalive_interval_millis) * kNanosPerMilli;
  next_keepalive_deadline_ = now + interval_nanos;

  if (idle_nanos < interval_nanos) {
    // 通道一直有数据在走，不需要保活。
    return Ok();
  }

  TETHERKITNEXT_ASSIGN_OR_RETURN(const std::uint32_t written,
                             EncodeKeepAlive(NextRequestId(), request_buffer_));
  auto exchange = Transact(std::span<const std::byte>{request_buffer_.data(), written},
                            MessageType::kKeepAliveComplete);
  if (!exchange) {
    ++consecutive_keepalive_failures_;
    TETHERKITNEXT_WARN_TR(Msg::kRndisKeepAliveFailed, consecutive_keepalive_failures_,
                      config_.keepalive_failure_threshold, exchange.error().ToString());
    if (consecutive_keepalive_failures_ >= config_.keepalive_failure_threshold) {
      Error fatal = std::move(exchange).error().WithContext(
          Tr(Msg::kRndisKeepAliveDeadLink, consecutive_keepalive_failures_));
      observer_->OnFatalError(fatal);
      return std::unexpected(std::move(fatal));
    }
    return Ok();  // 还没到阈值，下个周期再试
  }

  const auto complete = DecodeKeepAliveComplete(exchange->response);
  if (!complete) {
    ++consecutive_keepalive_failures_;
    return Ok();
  }

  if (complete->status != ToRaw(StatusCode::kSuccess)) {
    // 设备明确回了失败状态。这通常意味着设备想让我们复位。
    const std::string_view name = StatusName(complete->status);
    TETHERKITNEXT_WARN_TR(Msg::kRndisKeepAliveRejected,
                      name.empty() ? Text(Msg::kRndisUnknownStatus) : name);
    return Reset();
  }

  consecutive_keepalive_failures_ = 0;
  TETHERKITNEXT_TRACE_TR(Msg::kRndisKeepAliveOk);
  return Ok();
}

// =============================================================================
// 复位
// =============================================================================

Status StateMachine::Reset() {
  TETHERKITNEXT_INFO_TR(Msg::kRndisResetStarted);

  // RESET_MSG **没有 RequestId**（offset 8 是 Reserved），因此无法用 ID 配对，
  // 同一时刻只能有一个 RESET 在飞。
  TETHERKITNEXT_ASSIGN_OR_RETURN(const std::uint32_t written, EncodeReset(request_buffer_));
  TETHERKITNEXT_ASSIGN_OR_RETURN(
      const Exchange exchange,
      Transact(std::span<const std::byte>{request_buffer_.data(), written},
               MessageType::kResetComplete));

  TETHERKITNEXT_ASSIGN_OR_RETURN(const ResetComplete complete,
                             DecodeResetComplete(exchange.response));

  if (complete.status != ToRaw(StatusCode::kSuccess)) {
    const std::string_view name = StatusName(complete.status);
    return std::unexpected(Error::FromRndisStatus(
        complete.status, Tr(Msg::kRndisResetRejected,
                            name.empty() ? Text(Msg::kRndisUnknownStatus) : name)));
  }

  TETHERKITNEXT_INFO_TR(Msg::kRndisResetDone, Text(complete.addressing_reset
                                                    ? Msg::kRndisAddressingLost
                                                    : Msg::kRndisAddressingKept));
  observer_->OnDeviceReset(complete.addressing_reset);

  if (complete.addressing_reset) {
    // AddressingReset 非零 → 设备丢掉了包过滤与组播表，**必须重发**，
    // 否则复位后数据不会再流动。
    //
    // （本驱动不维护组播表 —— 包过滤里开了 ALL_MULTICAST，所以只需重发过滤器。
    //   如果将来加了 SET OID_802_3_MULTICAST_LIST，这里也要一并重放。）
    const std::uint32_t filter =
        active_packet_filter_ != 0 ? active_packet_filter_ : config_.packet_filter;
    TETHERKITNEXT_RETURN_IF_ERROR(SetOidUint32(Oid::kGenCurrentPacketFilter, filter));
    active_packet_filter_ = filter;
    TETHERKITNEXT_INFO_TR(Msg::kRndisPacketFilterReplayed, filter);
  }

  consecutive_keepalive_failures_ = 0;
  MarkDeviceActivity();
  return Ok();
}

// =============================================================================
// 停机
// =============================================================================

void StateMachine::SendHalt() noexcept {
  // HALT_MSG 设备**不会回复**，发完即可认为进入 uninitialized。
  const auto written = EncodeHalt(NextRequestId(), request_buffer_);
  if (!written) {
    TETHERKITNEXT_WARN_TR(Msg::kRndisEncodeHaltFailed, written.error().ToString());
    return;
  }
  if (const auto status =
          channel_->SendMessage(std::span<const std::byte>{request_buffer_.data(), *written});
      !status) {
    // 停机路径上的失败只记日志 —— 设备可能已经拔掉了，这很正常。
    TETHERKITNEXT_DEBUG_TR(Msg::kRndisSendHaltFailed, status.error().ToString());
    return;
  }
  TETHERKITNEXT_DEBUG_TR(Msg::kRndisHaltSent);
}

void StateMachine::Stop() {
  if (state_ == State::kUninitialized || state_ == State::kHalting) {
    return;
  }
  TransitionTo(State::kHalting);

  // 先把包过滤清零让设备停止发数据（规范：filter = 0 会让设备退回
  // RNDIS-initialized），再发 HALT。即使清零失败也要继续发 HALT ——
  // 不发的话设备会一直以为主机还在，下次插上时状态不干净。
  if (active_packet_filter_ != 0) {
    if (const auto status = SetOidUint32(Oid::kGenCurrentPacketFilter, 0); !status) {
      TETHERKITNEXT_DEBUG_TR(Msg::kRndisClearPacketFilterFailed, status.error().ToString());
    } else {
      active_packet_filter_ = 0;
    }
  }

  SendHalt();
  TransitionTo(State::kUninitialized);
}

}  // namespace tetherkitnext::rndis
