// 内存版 RNDIS 控制通道，用于离线驱动状态机。
//
// 它扮演「设备」这一侧：收下主机发来的请求，按脚本回复响应，还能主动插入
// INDICATE_STATUS 与设备发起的 KEEPALIVE —— 这些正是真机上极难复现、
// 却最容易写错的路径。
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <cstring>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/rndis/control_channel.h"
#include "tetherkitnext/rndis/messages.h"
#include "tetherkitnext/rndis/protocol.h"
#include "tetherkitnext/rndis/state_machine.h"

namespace tetherkitnext::testing {

/// 模拟设备行为的控制通道。
class MockControlChannel final : public rndis::ControlChannel {
 public:
  MockControlChannel() = default;

  // ---------------------------------------------------------------------------
  // ControlChannel 实现
  // ---------------------------------------------------------------------------

  [[nodiscard]] Status SendMessage(std::span<const std::byte> message) override {
    if (send_failure_countdown_ > 0) {
      --send_failure_countdown_;
      if (send_failure_countdown_ == 0) {
        return std::unexpected(Error::Generic("mock：按测试设置让发送失败"));
      }
    }
    sent_messages_.emplace_back(message.begin(), message.end());

    // 让「设备」根据收到的请求决定怎么回复。
    if (request_handler_) {
      request_handler_(message, *this);
    }
    return Ok();
  }

  [[nodiscard]] Result<std::span<const std::byte>> ReceiveMessage() override {
    ++receive_call_count_;
    if (receive_failure_countdown_ > 0) {
      --receive_failure_countdown_;
      if (receive_failure_countdown_ == 0) {
        return std::unexpected(Error::Generic("mock：按测试设置让接收失败"));
      }
    }
    if (pending_responses_.empty()) {
      // 与真实设备一致：没有响应时返回空（真机上是 1 字节 0x00）。
      return std::span<const std::byte>{};
    }
    current_response_ = std::move(pending_responses_.front());
    pending_responses_.pop_front();
    return std::span<const std::byte>{current_response_};
  }

  [[nodiscard]] rndis::NotificationResult WaitForNotification(
      std::uint32_t timeout_millis) override {
    ++notification_call_count_;
    // 记录下来供回归测试断言：**任何一次传 0 都是 bug** ——
    // libusb 在 darwin 上 timeout=0 表示无限等待，会把控制线程永久卡死。
    notification_timeouts_.push_back(timeout_millis);
    if (!has_interrupt_endpoint_) {
      return rndis::NotificationResult::kNotSupported;
    }
    return pending_responses_.empty() ? rndis::NotificationResult::kTimeout
                                      : rndis::NotificationResult::kResponseAvailable;
  }

  [[nodiscard]] std::uint32_t TimeoutMillis() const noexcept override { return 100; }

  [[nodiscard]] std::string_view Describe() const noexcept override { return description_; }

  // ---------------------------------------------------------------------------
  // 测试注入
  // ---------------------------------------------------------------------------

  /// 设置「设备」的请求处理逻辑。
  using RequestHandler =
      std::function<void(std::span<const std::byte> request, MockControlChannel& channel)>;

  void SetRequestHandler(RequestHandler handler) { request_handler_ = std::move(handler); }

  /// 排入一条设备 → 主机的消息。
  void EnqueueResponse(std::vector<std::byte> message) {
    pending_responses_.push_back(std::move(message));
  }

  /// 模拟设备没有中断端点（Linux 的 host 驱动就完全忽略它）。
  void SetHasInterruptEndpoint(bool has) noexcept { has_interrupt_endpoint_ = has; }

  /// 让第 N 次 SendMessage 失败（N 从 1 计）。
  void FailSendOnCall(std::uint32_t call_index) noexcept {
    send_failure_countdown_ = call_index;
  }

  /// 让第 N 次 ReceiveMessage 失败。
  void FailReceiveOnCall(std::uint32_t call_index) noexcept {
    receive_failure_countdown_ = call_index;
  }

  // ---------------------------------------------------------------------------
  // 观测
  // ---------------------------------------------------------------------------

  [[nodiscard]] const std::vector<std::vector<std::byte>>& SentMessages() const noexcept {
    return sent_messages_;
  }

  /// 主机发出的第 index 条消息的类型码。
  [[nodiscard]] std::uint32_t SentMessageType(std::size_t index) const {
    if (index >= sent_messages_.size() ||
        sent_messages_[index].size() < rndis::kMessageHeaderBytes) {
      return 0;
    }
    return LoadLe32(sent_messages_[index].data() + rndis::kMessageTypeOffset);
  }

  /// 统计主机一共发了多少条指定类型的消息。
  [[nodiscard]] std::size_t CountSent(rndis::MessageType type) const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < sent_messages_.size(); ++i) {
      if (SentMessageType(i) == rndis::ToRaw(type)) {
        ++count;
      }
    }
    return count;
  }

  /// 找出主机发的第一条 SET 消息，其 OID 等于给定值；返回它的 LE32 负载。
  [[nodiscard]] bool FindSetUint32(rndis::Oid oid, std::uint32_t& out_value,
                                   std::size_t skip = 0) const {
    for (const std::vector<std::byte>& message : sent_messages_) {
      if (message.size() < rndis::kQuerySetHeaderBytes + 4) {
        continue;
      }
      if (LoadLe32(message.data() + rndis::kMessageTypeOffset) !=
          rndis::ToRaw(rndis::MessageType::kSet)) {
        continue;
      }
      if (LoadLe32(message.data() + rndis::kQuerySetOidOffset) != rndis::ToRaw(oid)) {
        continue;
      }
      if (skip > 0) {
        --skip;
        continue;
      }
      out_value = LoadLe32(message.data() + rndis::kQuerySetHeaderBytes);
      return true;
    }
    return false;
  }

  [[nodiscard]] std::uint32_t ReceiveCallCount() const noexcept { return receive_call_count_; }

  /// 历次 WaitForNotification 收到的超时值。用于断言从不传 0。
  [[nodiscard]] const std::vector<std::uint32_t>& NotificationTimeouts() const noexcept {
    return notification_timeouts_;
  }

  [[nodiscard]] std::size_t PendingResponseCount() const noexcept {
    return pending_responses_.size();
  }

  void ClearSentMessages() { sent_messages_.clear(); }

 private:
  std::string description_ = "mock 设备";
  RequestHandler request_handler_;

  std::vector<std::vector<std::byte>> sent_messages_;
  std::deque<std::vector<std::byte>> pending_responses_;
  std::vector<std::byte> current_response_;

  bool has_interrupt_endpoint_ = true;
  std::uint32_t send_failure_countdown_ = 0;
  std::uint32_t receive_failure_countdown_ = 0;
  std::uint32_t receive_call_count_ = 0;
  std::uint32_t notification_call_count_ = 0;
  std::vector<std::uint32_t> notification_timeouts_;
};

// =============================================================================
// 构造设备回复的便捷函数
// =============================================================================

/// 造一条 INITIALIZE_CMPLT。
[[nodiscard]] inline std::vector<std::byte> MakeInitializeComplete(
    std::uint32_t request_id, std::uint32_t max_transfer_size = 2048,
    std::uint32_t max_packets = 1, std::uint32_t alignment_factor = 0,
    std::uint32_t status = 0) {
  std::vector<std::byte> message(rndis::kInitializeCmpltBytes);
  std::byte* base = message.data();
  StoreLe32(base + rndis::kMessageTypeOffset,
            rndis::ToRaw(rndis::MessageType::kInitializeComplete));
  StoreLe32(base + rndis::kMessageLengthOffset, rndis::kInitializeCmpltBytes);
  StoreLe32(base + rndis::kInitializeCmpltRequestIdOffset, request_id);
  StoreLe32(base + rndis::kInitializeCmpltStatusOffset, status);
  StoreLe32(base + rndis::kInitializeCmpltMajorVersionOffset, rndis::kMajorVersion);
  StoreLe32(base + rndis::kInitializeCmpltMinorVersionOffset, rndis::kMinorVersion);
  StoreLe32(base + rndis::kInitializeCmpltDeviceFlagsOffset,
            static_cast<std::uint32_t>(rndis::DeviceFlags::kConnectionless));
  StoreLe32(base + rndis::kInitializeCmpltMediumOffset,
            static_cast<std::uint32_t>(rndis::Medium::kEthernet));
  StoreLe32(base + rndis::kInitializeCmpltMaxPacketsPerMessageOffset, max_packets);
  StoreLe32(base + rndis::kInitializeCmpltMaxTransferSizeOffset, max_transfer_size);
  StoreLe32(base + rndis::kInitializeCmpltPacketAlignmentFactorOffset, alignment_factor);
  return message;
}

/// 造一条 QUERY_CMPLT，负载是任意字节。
[[nodiscard]] inline std::vector<std::byte> MakeQueryComplete(
    std::uint32_t request_id, std::span<const std::byte> payload, std::uint32_t status = 0) {
  const auto payload_length = static_cast<std::uint32_t>(payload.size());
  const std::uint32_t total = rndis::kQueryCmpltHeaderBytes + payload_length;
  std::vector<std::byte> message(total);
  std::byte* base = message.data();
  StoreLe32(base + rndis::kMessageTypeOffset, rndis::ToRaw(rndis::MessageType::kQueryComplete));
  StoreLe32(base + rndis::kMessageLengthOffset, total);
  StoreLe32(base + rndis::kQueryCmpltRequestIdOffset, request_id);
  StoreLe32(base + rndis::kQueryCmpltStatusOffset, status);
  StoreLe32(base + rndis::kQueryCmpltInfoBufferLengthOffset, payload_length);
  StoreLe32(base + rndis::kQueryCmpltInfoBufferOffsetOffset,
            payload_length == 0 ? 0 : rndis::kQueryCmpltInlineInfoOffset);
  if (payload_length != 0) {
    std::memcpy(base + rndis::kQueryCmpltHeaderBytes, payload.data(), payload_length);
  }
  return message;
}

/// 造一条 QUERY_CMPLT，负载是一个 LE32。
[[nodiscard]] inline std::vector<std::byte> MakeQueryCompleteUint32(std::uint32_t request_id,
                                                                   std::uint32_t value) {
  std::array<std::byte, 4> payload{};
  StoreLe32(payload.data(), value);
  return MakeQueryComplete(request_id, payload);
}

/// 造一条 QUERY_CMPLT，负载是 6 字节 MAC。
[[nodiscard]] inline std::vector<std::byte> MakeQueryCompleteMac(std::uint32_t request_id,
                                                                const rndis::MacAddress& mac) {
  std::array<std::byte, 6> payload{};
  std::memcpy(payload.data(), mac.data(), mac.size());
  return MakeQueryComplete(request_id, payload);
}

/// 造一条 SET_CMPLT。
[[nodiscard]] inline std::vector<std::byte> MakeSetComplete(std::uint32_t request_id,
                                                           std::uint32_t status = 0) {
  std::vector<std::byte> message(rndis::kSetCmpltBytes);
  std::byte* base = message.data();
  StoreLe32(base + rndis::kMessageTypeOffset, rndis::ToRaw(rndis::MessageType::kSetComplete));
  StoreLe32(base + rndis::kMessageLengthOffset, rndis::kSetCmpltBytes);
  StoreLe32(base + rndis::kSetCmpltRequestIdOffset, request_id);
  StoreLe32(base + rndis::kSetCmpltStatusOffset, status);
  return message;
}

/// 造一条 KEEPALIVE_CMPLT。
[[nodiscard]] inline std::vector<std::byte> MakeKeepAliveComplete(std::uint32_t request_id,
                                                                 std::uint32_t status = 0) {
  std::vector<std::byte> message(rndis::kKeepAliveCmpltBytes);
  std::byte* base = message.data();
  StoreLe32(base + rndis::kMessageTypeOffset,
            rndis::ToRaw(rndis::MessageType::kKeepAliveComplete));
  StoreLe32(base + rndis::kMessageLengthOffset, rndis::kKeepAliveCmpltBytes);
  StoreLe32(base + rndis::kKeepAliveCmpltRequestIdOffset, request_id);
  StoreLe32(base + rndis::kKeepAliveCmpltStatusOffset, status);
  return message;
}

/// 造一条**设备主动发起**的 KEEPALIVE_MSG（主机必须回 CMPLT）。
[[nodiscard]] inline std::vector<std::byte> MakeDeviceKeepAlive(std::uint32_t request_id) {
  std::vector<std::byte> message(rndis::kKeepAliveMsgBytes);
  std::byte* base = message.data();
  StoreLe32(base + rndis::kMessageTypeOffset, rndis::ToRaw(rndis::MessageType::kKeepAlive));
  StoreLe32(base + rndis::kMessageLengthOffset, rndis::kKeepAliveMsgBytes);
  StoreLe32(base + rndis::kKeepAliveRequestIdOffset, request_id);
  return message;
}

/// 造一条 RESET_CMPLT。注意它**没有 RequestId**，Status 在 offset 8。
[[nodiscard]] inline std::vector<std::byte> MakeResetComplete(bool addressing_reset,
                                                             std::uint32_t status = 0) {
  std::vector<std::byte> message(rndis::kResetCmpltBytes);
  std::byte* base = message.data();
  StoreLe32(base + rndis::kMessageTypeOffset, rndis::ToRaw(rndis::MessageType::kResetComplete));
  StoreLe32(base + rndis::kMessageLengthOffset, rndis::kResetCmpltBytes);
  StoreLe32(base + rndis::kResetCmpltStatusOffset, status);
  StoreLe32(base + rndis::kResetCmpltAddressingResetOffset, addressing_reset ? 1 : 0);
  return message;
}

/// 造一条 INDICATE_STATUS（无负载）。
[[nodiscard]] inline std::vector<std::byte> MakeIndicateStatus(rndis::StatusCode status) {
  std::vector<std::byte> message(rndis::kIndicateStatusHeaderBytes);
  std::byte* base = message.data();
  StoreLe32(base + rndis::kMessageTypeOffset, rndis::ToRaw(rndis::MessageType::kIndicateStatus));
  StoreLe32(base + rndis::kMessageLengthOffset, rndis::kIndicateStatusHeaderBytes);
  StoreLe32(base + rndis::kIndicateStatusStatusOffset, rndis::ToRaw(status));
  StoreLe32(base + rndis::kIndicateStatusBufferLengthOffset, 0);
  StoreLe32(base + rndis::kIndicateStatusBufferOffsetOffset, 0);
  return message;
}

/// 从主机发来的请求里取 RequestId（QUERY/SET/INITIALIZE/KEEPALIVE 都在 offset 8）。
[[nodiscard]] inline std::uint32_t RequestIdOf(std::span<const std::byte> request) {
  if (request.size() < 12) {
    return 0;
  }
  return LoadLe32(request.data() + 8);
}

/// 从主机发来的 QUERY/SET 请求里取 OID。
[[nodiscard]] inline std::uint32_t OidOf(std::span<const std::byte> request) {
  if (request.size() < rndis::kQuerySetHeaderBytes) {
    return 0;
  }
  return LoadLe32(request.data() + rndis::kQuerySetOidOffset);
}

/// 一个「行为正常的 Android 设备」的请求处理逻辑。
///
/// 覆盖启动序列需要的全部 OID，未知的 OID 一律回 NOT_SUPPORTED（真机也是这样）。
[[nodiscard]] inline MockControlChannel::RequestHandler MakeWellBehavedDevice(
    const rndis::MacAddress& mac, std::uint32_t max_transfer_size = 2048,
    std::uint32_t max_packets = 1, std::uint32_t alignment_factor = 0) {
  return [mac, max_transfer_size, max_packets, alignment_factor](
             std::span<const std::byte> request, MockControlChannel& channel) {
    if (request.size() < rndis::kMessageHeaderBytes) {
      return;
    }
    const std::uint32_t type = LoadLe32(request.data() + rndis::kMessageTypeOffset);
    const std::uint32_t request_id = RequestIdOf(request);

    if (type == rndis::ToRaw(rndis::MessageType::kInitialize)) {
      channel.EnqueueResponse(MakeInitializeComplete(request_id, max_transfer_size, max_packets,
                                                     alignment_factor));
      return;
    }
    if (type == rndis::ToRaw(rndis::MessageType::kQuery)) {
      const std::uint32_t oid = OidOf(request);
      if (oid == rndis::ToRaw(rndis::Oid::kEthernetPermanentAddress) ||
          oid == rndis::ToRaw(rndis::Oid::kEthernetCurrentAddress)) {
        channel.EnqueueResponse(MakeQueryCompleteMac(request_id, mac));
      } else if (oid == rndis::ToRaw(rndis::Oid::kGenMaximumFrameSize)) {
        channel.EnqueueResponse(MakeQueryCompleteUint32(request_id, 1500));
      } else if (oid == rndis::ToRaw(rndis::Oid::kGenLinkSpeed)) {
        // 单位是 100 bps：4800000 → 480 Mbps
        channel.EnqueueResponse(MakeQueryCompleteUint32(request_id, 4'800'000));
      } else if (oid == rndis::ToRaw(rndis::Oid::kGenMediaConnectStatus)) {
        // 0 = 已连接
        channel.EnqueueResponse(MakeQueryCompleteUint32(
            request_id, static_cast<std::uint32_t>(rndis::MediaState::kConnected)));
      } else if (oid == rndis::ToRaw(rndis::Oid::kGenVendorId)) {
        channel.EnqueueResponse(MakeQueryCompleteUint32(request_id, 0x0018D1));
      } else {
        // 未知 / 可选 OID：回不支持。主线 Linux gadget 也是这样。
        channel.EnqueueResponse(MakeQueryComplete(request_id, {},
                                                  rndis::ToRaw(rndis::StatusCode::kNotSupported)));
      }
      return;
    }
    if (type == rndis::ToRaw(rndis::MessageType::kSet)) {
      channel.EnqueueResponse(MakeSetComplete(request_id));
      return;
    }
    if (type == rndis::ToRaw(rndis::MessageType::kKeepAlive)) {
      channel.EnqueueResponse(MakeKeepAliveComplete(request_id));
      return;
    }
    if (type == rndis::ToRaw(rndis::MessageType::kReset)) {
      channel.EnqueueResponse(MakeResetComplete(/*addressing_reset=*/true));
      return;
    }
    // HALT_MSG 设备不回复。
  };
}

/// 记录状态机事件的观察者。
class RecordingObserver final : public rndis::StateMachineObserver {
 public:
  struct Transition {
    rndis::State from;
    rndis::State to;
  };

  void OnStateChanged(rndis::State from, rndis::State to) override {
    transitions.push_back(Transition{.from = from, .to = to});
  }

  void OnNegotiated(const rndis::NegotiatedParameters& parameters,
                    const rndis::DeviceInfo& info) override {
    ++negotiated_count;
    parameters_snapshot = parameters;
    info_snapshot = info;
  }

  void OnLinkStateChanged(bool connected) override { link_events.push_back(connected); }

  void OnDeviceReset(bool addressing_lost) override { reset_events.push_back(addressing_lost); }

  void OnFatalError(const Error& error) override { fatal_errors.push_back(error.ToString()); }

  std::vector<Transition> transitions;
  std::vector<bool> link_events;
  std::vector<bool> reset_events;
  std::vector<std::string> fatal_errors;
  std::uint32_t negotiated_count = 0;
  rndis::NegotiatedParameters parameters_snapshot{};
  rndis::DeviceInfo info_snapshot{};
};

}  // namespace tetherkitnext::testing
