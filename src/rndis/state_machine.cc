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

/// Size of the placeholder buffer given when QUERYing a fixed-length OID.
///
/// For OIDs that return fixed-length results, InformationBufferLength must be >= the expected response length, otherwise Microsoft's
/// ActiveSync implementation replies RNDIS_STATUS_INVALID_LENGTH. Linux gives 48 when querying the MAC,
/// and the same "loose but sufficient" value is reused here as the default placeholder for most fixed-length OIDs.
constexpr std::uint32_t kFixedOidPlaceholderBytes = 48;

/// Extracts a string from an ASCII buffer that may not be NUL-terminated.
[[nodiscard]] std::string ToPrintableString(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    const auto character = static_cast<char>(byte);
    if (character == '\0') {
      break;  // some devices really do include a NUL; stop when encountered
    }
    // Filter out non-printable characters, to avoid printing control characters into the log.
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
  // Skip 0: some devices treat RequestId == 0 as an invalid value (FreeBSD's implementation simply fills in all 0s,
  // which shows that devices generally do not validate it, but to be conservative we avoid it anyway).
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
// Control channel round trip
// =============================================================================

Result<bool> StateMachine::PumpOnce(std::optional<MessageType> expected_reply,
                                   std::span<const std::byte>& out_response) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const std::span<const std::byte> message,
                             channel_->ReceiveMessage());

  if (message.empty()) {
    // Spec: when the device has no valid response yet it returns 1 byte of 0x00 rather than a STALL. Not an error.
    return false;
  }
  MarkDeviceActivity();

  TETHERKITNEXT_ASSIGN_OR_RETURN(const MessageHeader header, DecodeMessageHeader(message));
  const std::string_view name = MessageTypeName(header.message_type);
  TETHERKITNEXT_DEBUG_TR(Msg::kRndisControlMessageReceived,
                     name.empty() ? Text(Msg::kRndisUnknownMessage) : name, header.message_type,
                     header.message_length);

  // ---- Messages actively pushed by the device: dispatch them away and keep waiting for the one we want ----
  if (header.message_type == ToRaw(MessageType::kIndicateStatus)) {
    HandleIndicateStatus(message);
    return false;
  }
  if (header.message_type == ToRaw(MessageType::kKeepAlive)) {
    // Device-initiated keepalive. **Must reply**, otherwise the device may judge the host dead and disconnect.
    TETHERKITNEXT_RETURN_IF_ERROR(HandleDeviceKeepAlive(message));
    return false;
  }

  // ---- Messages of connection-oriented devices: explicitly report unsupported, rather than a vague "unknown message" ----
  if (IsCondis(header.message_type)) {
    return std::unexpected(Error::Generic(Tr(Msg::kRndisCondisMessageReceived,
                                             name.empty() ? Text(Msg::kRndisUnknown) : name)));
  }

  // ---- Is it the one we are waiting for? ----
  if (expected_reply.has_value() && header.message_type == ToRaw(*expected_reply)) {
    out_response = message;
    return true;
  }

  // Not the one we are waiting for, and not a known push message either. Possibly a late response to a previous timed-out request --
  // discard it and continue (it cannot be treated as a fatal error, otherwise a single timeout would judge the link dead).
  TETHERKITNEXT_WARN_TR(Msg::kRndisDiscardUnexpectedMessage,
                    name.empty() ? Text(Msg::kRndisUnknown) : name, header.message_type);
  return false;
}

Result<StateMachine::Exchange> StateMachine::Transact(std::span<const std::byte> request,
                                                      MessageType expected_reply) {
  TETHERKITNEXT_RETURN_IF_ERROR(channel_->SendMessage(request));

  // Wait first for the RESPONSE_AVAILABLE notification on the interrupt endpoint, then fetch the response.
  //
  // Both kinds of device behavior must be supported:
  //   * The spec way is to wait for the notification;
  //   * But there are also devices that "must have the interrupt endpoint read once before they answer on the control endpoint";
  //   * And Linux simply ignores the interrupt endpoint entirely and polls the control endpoint directly.
  // So: if there is an interrupt endpoint, wait for a notification once first (a timeout is not an error either), and then poll regardless.
  // It likewise cannot be 0 (timeout=0 is an infinite wait on darwin). Note that response_poll_interval
  // can be configured to 0 (it is in the tests), so max must be used as a backstop here.
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
    // Not ready yet. Sleep a little and try again -- it must sleep here, otherwise it would turn the control endpoint into busy polling,
    // and on Apple Silicon each control transfer is itself a millisecond-level cost.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.response_poll_interval_millis));
  }

  const std::string_view name = MessageTypeName(ToRaw(expected_reply));
  return std::unexpected(Error::Generic(
      Tr(Msg::kRndisWaitReplyTimeout, name.empty() ? Text(Msg::kRndisResponse) : name,
         config_.response_poll_attempts, config_.response_poll_interval_millis)));
}

// =============================================================================
// Handling of device-pushed messages
// =============================================================================

void StateMachine::HandleIndicateStatus(std::span<const std::byte> message) {
  const auto indication = DecodeIndicateStatus(message);
  if (!indication) {
    // A parse failure should not kill the link -- this is a purely informational message.
    TETHERKITNEXT_WARN_TR(Msg::kRndisIndicateStatusParseFailed, indication.error().ToString());
    return;
  }

  const std::string_view status_name = StatusName(indication->status);
  if (indication->has_diagnostic_info) {
    // The device uses Rndis_Diagnostic_Info to tell us "byte N of the message you sent is invalid",
    // which is highly valuable for tracking down our own encoding bugs, so it is printed separately.
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
      // Other statuses are only logged. In particular kInvalidData -- it means the message we sent has a problem,
      // but the link should not be dropped on that basis (the diagnostic information has already been printed).
      break;
  }
}

Status StateMachine::HandleDeviceKeepAlive(std::span<const std::byte> message) {
  // The RequestId of a device-initiated KEEPALIVE_MSG is at offset 8, the same layout as when initiated by the host.
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
// OID reads and writes
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
      // An optional OID returning NOT_SUPPORTED is completely normal (for example OID_GEN_PHYSICAL_MEDIUM).
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
// Startup sequence
// =============================================================================

Status StateMachine::CollectDeviceInfo() {
  // ---- Physical medium: **optional OID**, failure is not fatal ----
  // Linux passes in_len of 4 when querying it, and on failure just continues as UNSPECIFIED.
  if (const auto medium = QueryOid(Oid::kGenPhysicalMedium, 4, /*fatal=*/false);
      medium && !medium->empty()) {
    if (const auto value = ParseUint32(*medium)) {
      info_.physical_medium = static_cast<PhysicalMedium>(*value);
    }
  }

  // ---- Permanent MAC: **fatal** ----
  // This is the address the host-side feth is to adopt; without it the NIC cannot be built correctly.
  {
    TETHERKITNEXT_ASSIGN_OR_RETURN(
        const std::span<const std::byte> payload,
        QueryOid(Oid::kEthernetPermanentAddress, kFixedOidPlaceholderBytes, /*fatal=*/true));
    TETHERKITNEXT_ASSIGN_OR_RETURN(info_.permanent_address, ParseMac(payload));
    info_.has_permanent_address = true;
    TETHERKITNEXT_INFO_TR(Msg::kRndisPermanentMac, FormatMac(info_.permanent_address).data());
  }

  // ---- Current MAC: not fatal (the same as the permanent MAC on most devices) ----
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

  // ---- Maximum frame length: not fatal (the negotiation result already gave the MTU) ----
  if (const auto payload =
          QueryOid(Oid::kGenMaximumFrameSize, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.maximum_frame_size = *value;
      // The device says it can only receive at most N bytes of payload, while the MTU we negotiated is larger -- listen to the device.
      if (*value != 0 && *value < parameters_.mtu) {
        TETHERKITNEXT_WARN_TR(Msg::kRndisMtuLoweredToDeviceFrameSize, *value, parameters_.mtu);
        parameters_.mtu = *value;
      }
    }
  }

  // ---- Link speed: not fatal, used only for logging ----
  if (const auto payload = QueryOid(Oid::kGenLinkSpeed, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.link_speed_100bps = *value;
    }
  }

  // ---- Media connect status: not fatal. **Note that 0 means connected** ----
  if (const auto payload = QueryOid(Oid::kGenMediaConnectStatus, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.media_state = static_cast<MediaState>(*value);
      link_connected_ = info_.media_state == MediaState::kConnected;
    }
  } else {
    // If it cannot be queried, assume connected -- since the device is running RNDIS, the link is most likely up.
    link_connected_ = true;
  }

  // ---- Vendor information: purely for diagnostics ----
  if (const auto payload = QueryOid(Oid::kGenVendorId, 4, /*fatal=*/false);
      payload && !payload->empty()) {
    if (const auto value = ParseUint32(*payload)) {
      info_.vendor_id = *value;
    }
  }
  // The vendor description is a **variable-length** OID; expected_bytes must be passed as 0.
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

  // ---- Step 1: INITIALIZE ----
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

    // The aggregated packet count the device reports is only its **claim**; the caller may distrust it (see the config item comments).
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

  // ---- Steps 2~4: collect device information ----
  if (const auto status = CollectDeviceInfo(); !status) {
    SendHalt();
    TransitionTo(State::kUninitialized);
    return status;
  }

  // ---- Step 5: set the packet filter -> the device enters data-initialized and data starts flowing ----
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
// Periodic work
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
    return Ok();  // not ready or shutting down, nothing to do
  }

  // ---- First drain the messages actively pushed by the device ----
  //
  // If not drained they pile up in the device's response queue, pushing the responses to our later requests further back,
  // so every Transact has to loop several more times to get the one it wants.
  //
  // Read the control endpoint only when there is an interrupt notification -- otherwise every Poll would have to send one
  // GET_ENCAPSULATED_RESPONSE, which is a millisecond-level cost on Apple Silicon.
  //
  // WARNING: **Never pass 0 here**: on darwin libusb a timeout=0 means wait forever,
  // which would hang the control thread permanently. Use kProbeOnlyTimeoutMillis (1 ms) --
  // Poll itself only runs once every few hundred milliseconds, so a 1 ms block can be ignored.
  if (channel_->WaitForNotification(kProbeOnlyTimeoutMillis) ==
      NotificationResult::kResponseAvailable) {
    std::span<const std::byte> ignored;
    // Drain at most 8, to avoid getting stuck here when the device pushes like crazy.
    for (int i = 0; i < 8; ++i) {
      const auto pumped = PumpOnce(std::nullopt, ignored);
      if (!pumped) {
        return std::unexpected(
            Error{pumped.error()}.WithContext(Tr(Msg::kRndisDrainControlFailed)));
      }
      // PumpOnce returns false without an error when there are no more messages, so it cannot distinguish "empty" from
      // "handled one push". Judge once more using the notification state.
      if (channel_->WaitForNotification(kProbeOnlyTimeoutMillis) !=
          NotificationResult::kResponseAvailable) {
        break;
      }
    }
  }

  // ---- Keepalive ----
  //
  // The semantics are to send only when "more than the keepalive period has passed since any message was last received from the device", not to send unconditionally on a timer.
  // Pointless keepalive round trips while the channel is active are purely wasteful (especially on Apple Silicon).
  const Nanos now = MonotonicNanos();
  if (now < next_keepalive_deadline_) {
    return Ok();
  }

  const Nanos idle_nanos = now - last_device_activity_;
  const Nanos interval_nanos =
      static_cast<Nanos>(config_.keepalive_interval_millis) * kNanosPerMilli;
  next_keepalive_deadline_ = now + interval_nanos;

  if (idle_nanos < interval_nanos) {
    // The channel has had data moving all along; no keepalive is needed.
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
    return Ok();  // threshold not reached yet, try again next period
  }

  const auto complete = DecodeKeepAliveComplete(exchange->response);
  if (!complete) {
    ++consecutive_keepalive_failures_;
    return Ok();
  }

  if (complete->status != ToRaw(StatusCode::kSuccess)) {
    // The device explicitly replied with a failure status. This usually means the device wants us to reset.
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
// Reset
// =============================================================================

Status StateMachine::Reset() {
  TETHERKITNEXT_INFO_TR(Msg::kRndisResetStarted);

  // RESET_MSG **has no RequestId** (offset 8 is Reserved), so it cannot be paired by ID,
  // and only one RESET can be in flight at a time.
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
    // AddressingReset non-zero -> the device dropped the packet filter and multicast table, and **it must be resent**,
    // otherwise data will not flow again after the reset.
    //
    // (This driver does not maintain a multicast table -- ALL_MULTICAST is enabled in the packet filter, so only the filter needs resending.
    //   If SET OID_802_3_MULTICAST_LIST is added in the future, it must be replayed here too.)
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
// Shutdown
// =============================================================================

void StateMachine::SendHalt() noexcept {
  // The device **does not reply** to HALT_MSG; once sent it can be considered to have entered uninitialized.
  const auto written = EncodeHalt(NextRequestId(), request_buffer_);
  if (!written) {
    TETHERKITNEXT_WARN_TR(Msg::kRndisEncodeHaltFailed, written.error().ToString());
    return;
  }
  if (const auto status =
          channel_->SendMessage(std::span<const std::byte>{request_buffer_.data(), *written});
      !status) {
    // Failures on the shutdown path are only logged -- the device may already have been unplugged, which is quite normal.
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

  // First clear the packet filter to make the device stop sending data (spec: filter = 0 makes the device fall back to
  // RNDIS-initialized), then send HALT. Continue to send HALT even if clearing fails --
  // if it is not sent, the device will keep thinking the host is still there, and the state will not be clean the next time it is plugged in.
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
