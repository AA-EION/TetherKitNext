// Unit tests of the RNDIS state machine.
//
// The state machine is the most logically complex part of the whole protocol implementation, and many of its paths are extremely hard to reproduce on real hardware
// (device cut-in pushes, keepalive failures, replay required after a reset). Here MockControlChannel plays
// the device side, covering all of these paths.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest.h>

#include "language_guard.h"

#include "mock_control_channel.h"
#include "tetherkitnext/rndis/state_machine.h"

// Same reason as the same-named helper in test_net_link.cc: writing directly inside REQUIRE_MESSAGE
// `r ? "" : r.error().ToString()` does not compile -- doctest's MessageBuilder swallows
// the whole ternary expression into the stream and then tries to convert it to bool.
namespace {
template <typename T>
std::string Why(const T& result) {
  return result.has_value() ? std::string{} : result.error().ToString();
}
}  // namespace

using namespace tetherkitnext;                   // NOLINT(google-build-using-namespace)
using namespace tetherkitnext::rndis;            // NOLINT(google-build-using-namespace)
using tetherkitnext::testing::MakeDeviceKeepAlive;
using tetherkitnext::testing::MakeIndicateStatus;
using tetherkitnext::testing::MakeInitializeComplete;
using tetherkitnext::testing::MakeKeepAliveComplete;
using tetherkitnext::testing::MakeQueryComplete;
using tetherkitnext::testing::MakeQueryCompleteMac;
using tetherkitnext::testing::MakeQueryCompleteUint32;
using tetherkitnext::testing::MakeResetComplete;
using tetherkitnext::testing::MakeSetComplete;
using tetherkitnext::testing::MakeWellBehavedDevice;
using tetherkitnext::testing::MockControlChannel;
using tetherkitnext::testing::OidOf;
using tetherkitnext::testing::RecordingObserver;
using tetherkitnext::testing::RequestIdOf;

namespace {

constexpr MacAddress kDeviceMac{0x02, 0x1A, 0x11, 0x22, 0x33, 0x44};

/// Test configuration: presses the polling interval down to 0, making tests run fast.
[[nodiscard]] StateMachineConfig FastConfig() {
  StateMachineConfig config;
  config.response_poll_interval_millis = 0;
  config.response_poll_attempts = 4;
  config.keepalive_interval_millis = 0;  // makes the keepalive expire immediately, easing testing
  return config;
}

}  // namespace

TEST_SUITE("rndis.state_machine") {

TEST_CASE("状态名齐全") {
  CHECK_FALSE(StateName(State::kUninitialized).empty());
  CHECK_FALSE(StateName(State::kInitializing).empty());
  CHECK_FALSE(StateName(State::kInitialized).empty());
  CHECK_FALSE(StateName(State::kDataInitialized).empty());
  CHECK_FALSE(StateName(State::kHalting).empty());
}

TEST_CASE("完整启动序列：走到 kDataInitialized 并通报协商结果") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  CHECK(machine.CurrentState() == State::kUninitialized);
  const auto status = machine.Start();
  REQUIRE_MESSAGE(status.has_value(), Why(status));

  CHECK(machine.CurrentState() == State::kDataInitialized);

  // The state transition path must be uninitialized -> initializing -> initialized -> data ready.
  REQUIRE(observer.transitions.size() == 3);
  CHECK(observer.transitions[0].to == State::kInitializing);
  CHECK(observer.transitions[1].to == State::kInitialized);
  CHECK(observer.transitions[2].to == State::kDataInitialized);

  CHECK(observer.negotiated_count == 1);
  CHECK(observer.parameters_snapshot.mtu == 1500);
  CHECK(observer.parameters_snapshot.device_max_transfer_size == 2048);
  CHECK(observer.parameters_snapshot.max_packets_per_message == 1);
  CHECK(observer.parameters_snapshot.tx_alignment_bytes == 1);

  // Got the device MAC -- this is the address the host-side feth is to use.
  CHECK(machine.Info().has_permanent_address);
  CHECK(machine.Info().permanent_address == kDeviceMac);
  CHECK(machine.Info().LinkSpeedMbps() == doctest::Approx(480.0));

  // The link reported "connected" once.
  REQUIRE(observer.link_events.size() == 1);
  CHECK(observer.link_events[0]);
}

TEST_CASE("启动序列的消息顺序：INITIALIZE 在最前，SET 包过滤在最后") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  REQUIRE_FALSE(channel.SentMessages().empty());
  // The first must be INITIALIZE_MSG.
  CHECK(channel.SentMessageType(0) == ToRaw(MessageType::kInitialize));
  // The last must be SET (packet filter) -- only it puts the device into data-initialized.
  const std::size_t last = channel.SentMessages().size() - 1;
  CHECK(channel.SentMessageType(last) == ToRaw(MessageType::kSet));

  // Only one INITIALIZE was sent, with no repeats.
  CHECK(channel.CountSent(MessageType::kInitialize) == 1);

  // The packet filter value set must be the one we requested (DIRECTED|BROADCAST|ALL_MULTICAST|PROMISCUOUS).
  std::uint32_t filter = 0;
  REQUIRE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter));
  CHECK(filter == kDefaultPacketFilter);
  CHECK(filter == 0x0000'002DU);
}

TEST_CASE("变长 OID 的 InformationBufferLength 必须为 0，定长必须非 0") {
  MockControlChannel channel;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> query_oid_and_length;
  channel.SetRequestHandler(
      [&](std::span<const std::byte> request, MockControlChannel& mock) {
        const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
        if (type == ToRaw(MessageType::kQuery)) {
          query_oid_and_length.emplace_back(
              OidOf(request), LoadLe32(request.data() + kQuerySetInfoBufferLengthOffset));
        }
        MakeWellBehavedDevice(kDeviceMac)(request, mock);
      });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  bool saw_fixed = false;
  bool saw_variable = false;
  for (const auto& [oid, length] : query_oid_and_length) {
    if (IsVariableLengthOid(static_cast<Oid>(oid))) {
      // Passing a non-zero length for a variable-length OID gets rejected by the ActiveSync implementation.
      CHECK_MESSAGE(length == 0, "变长 OID " << OidName(oid) << " 的长度应为 0");
      saw_variable = true;
    } else {
      CHECK_MESSAGE(length > 0, "定长 OID " << OidName(oid) << " 的长度应大于 0");
      saw_fixed = true;
    }
  }
  CHECK(saw_fixed);
  CHECK(saw_variable);  // OID_GEN_VENDOR_DESCRIPTION was queried in the startup sequence
}

TEST_CASE("可选 OID 返回 NOT_SUPPORTED 不影响启动") {
  // OID_GEN_PHYSICAL_MEDIUM is optional, and the device replying unsupported is entirely normal.
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());
  CHECK(machine.CurrentState() == State::kDataInitialized);
  CHECK(machine.Info().physical_medium == PhysicalMedium::kUnspecified);
  CHECK(observer.fatal_errors.empty());
}

TEST_CASE("拿不到永久 MAC 是致命错误，且会发 HALT") {
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    const std::uint32_t request_id = RequestIdOf(request);
    if (type == ToRaw(MessageType::kInitialize)) {
      mock.EnqueueResponse(MakeInitializeComplete(request_id));
      return;
    }
    if (type == ToRaw(MessageType::kQuery)) {
      // All QUERYs reply unsupported -- including the permanent MAC.
      mock.EnqueueResponse(
          MakeQueryComplete(request_id, {}, ToRaw(StatusCode::kNotSupported)));
    }
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  const auto status = machine.Start();
  REQUIRE_FALSE(status.has_value());
  // After failure it must return to uninitialized, rather than get stuck in an intermediate state.
  CHECK(machine.CurrentState() == State::kUninitialized);
  // And HALT must have been sent to clean up the device-side state.
  CHECK(channel.CountSent(MessageType::kHalt) == 1);
}

TEST_CASE("设备拒绝初始化时不发 QUERY，直接失败") {
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kInitialize)) {
      mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request), 2048, 1, 0,
                                                  ToRaw(StatusCode::kFailure)));
    }
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  REQUIRE_FALSE(machine.Start().has_value());
  CHECK(machine.CurrentState() == State::kUninitialized);
  CHECK(channel.CountSent(MessageType::kQuery) == 0);
}

TEST_CASE("非以太网介质被拒绝") {
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    if (LoadLe32(request.data() + kMessageTypeOffset) == ToRaw(MessageType::kInitialize)) {
      auto message = MakeInitializeComplete(RequestIdOf(request));
      StoreLe32(message.data() + kInitializeCmpltMediumOffset,
                static_cast<std::uint32_t>(Medium::kWirelessLan));
      mock.EnqueueResponse(std::move(message));
    }
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE_FALSE(machine.Start().has_value());
}

TEST_CASE("设备在等响应期间插入 INDICATE_STATUS，不影响请求配对") {
  // This is a path that is very common on real hardware but extremely hard to reproduce on purpose.
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kInitialize)) {
      // First stuff in a media-disconnect report, then the real response.
      mock.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
      mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request)));
      return;
    }
    MakeWellBehavedDevice(kDeviceMac)(request, mock);
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  const auto status = machine.Start();
  REQUIRE_MESSAGE(status.has_value(), Why(status));
  CHECK(machine.CurrentState() == State::kDataInitialized);

  // The cut-in message must be consumed and not pile up in the queue crowding out later responses.
  CHECK(channel.PendingResponseCount() == 0);

  // About link events: link state is **edge-triggered**. The initial state assumed internally at startup is
  // "not connected", so a MEDIA_DISCONNECT matches the current understanding -> correctly produces no event
  // (deduplication). Afterwards QUERY OID_GEN_MEDIA_CONNECT_STATUS returns 0 (connected),
  // which is the newer, authoritative state, so Start() reports "connected" once at the end.
  REQUIRE(observer.link_events.size() == 1);
  CHECK(observer.link_events[0]);
}

TEST_CASE("链路已 up 之后收到 MEDIA_DISCONNECT 才产生断开事件") {
  // This is the scenario where MEDIA_DISCONNECT really should trigger an event: there is an up -> down edge.
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  auto config = FastConfig();
  config.keepalive_interval_millis = 60'000;  // do not let keepalive interfere
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());

  // After startup the link is up.
  REQUIRE(observer.link_events.size() == 1);
  REQUIRE(observer.link_events[0]);

  // The device now pushes a MEDIA_DISCONNECT; Poll should take it away and report the disconnect.
  channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
  REQUIRE(machine.Poll().has_value());

  REQUIRE(observer.link_events.size() == 2);
  CHECK_FALSE(observer.link_events[1]);

  // Push another identical report -- edge-triggered, it should not produce another event.
  channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
  REQUIRE(machine.Poll().has_value());
  CHECK(observer.link_events.size() == 2);

  // On restoring the connection a down -> up edge should be produced.
  channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaConnect));
  REQUIRE(machine.Poll().has_value());
  REQUIRE(observer.link_events.size() == 3);
  CHECK(observer.link_events[2]);
}

TEST_CASE("设备主动发 KEEPALIVE_MSG 时主机必须回 KEEPALIVE_CMPLT") {
  // If it does not reply, the device may judge the host dead and disconnect.
  MockControlChannel channel;
  constexpr std::uint32_t kDeviceKeepAliveId = 0xABCD'1234;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kInitialize)) {
      // Insert a device-initiated keepalive before the response.
      mock.EnqueueResponse(MakeDeviceKeepAlive(kDeviceKeepAliveId));
      mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request)));
      return;
    }
    MakeWellBehavedDevice(kDeviceMac)(request, mock);
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  // The host must have sent a KEEPALIVE_CMPLT, and its RequestId is the one the device gave.
  bool found = false;
  for (const std::vector<std::byte>& message : channel.SentMessages()) {
    if (message.size() >= kKeepAliveCmpltBytes &&
        LoadLe32(message.data() + kMessageTypeOffset) ==
            ToRaw(MessageType::kKeepAliveComplete)) {
      CHECK(LoadLe32(message.data() + kKeepAliveCmpltRequestIdOffset) == kDeviceKeepAliveId);
      CHECK(LoadLe32(message.data() + kKeepAliveCmpltStatusOffset) ==
            ToRaw(StatusCode::kSuccess));
      found = true;
    }
  }
  CHECK_MESSAGE(found, "主机没有回复设备发起的 KEEPALIVE");
}

TEST_CASE("面向连接设备的消息被明确拒绝") {
  // The following asserts the Chinese wording of the error message, so pin the language first.
  const tetherkitnext::testing::ScopedLanguage guard{tetherkitnext::Language::kChinese};
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    if (LoadLe32(request.data() + kMessageTypeOffset) == ToRaw(MessageType::kInitialize)) {
      // Stuff in a CONDIS message.
      std::vector<std::byte> condis(kMessageHeaderBytes);
      StoreLe32(condis.data() + kMessageTypeOffset, 0x0000'8001U);  // MP_CREATE_VC
      StoreLe32(condis.data() + kMessageLengthOffset, kMessageHeaderBytes);
      mock.EnqueueResponse(std::move(condis));
      mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request)));
    }
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  const auto status = machine.Start();
  REQUIRE_FALSE(status.has_value());
  // The error message must explicitly point out that it is a connection-oriented device, rather than a vague "unknown message".
  CHECK(status.error().ToString().find("面向连接") != std::string::npos);
}

TEST_CASE("保活正常时不报错，且设备活跃期间不发无谓保活") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  auto config = FastConfig();
  // Keepalive period set very large: Poll should not send a keepalive.
  config.keepalive_interval_millis = 60'000;
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());

  channel.ClearSentMessages();
  REQUIRE(machine.Poll().has_value());
  CHECK(channel.CountSent(MessageType::kKeepAlive) == 0);
}

TEST_CASE("保活到期时发 KEEPALIVE 并在成功后清零失败计数") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  auto config = FastConfig();
  config.keepalive_interval_millis = 0;  // expires immediately
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());

  channel.ClearSentMessages();
  REQUIRE(machine.Poll().has_value());
  CHECK(channel.CountSent(MessageType::kKeepAlive) == 1);
  CHECK(observer.fatal_errors.empty());
}

TEST_CASE("连续保活失败达到阈值后通报致命错误") {
  // The following asserts the Chinese wording in the fatal error, so pin the language first.
  const tetherkitnext::testing::ScopedLanguage guard{tetherkitnext::Language::kChinese};
  MockControlChannel channel;
  // The device replies only to INITIALIZE / QUERY / SET, and never replies to KEEPALIVE -> times out every time.
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kKeepAlive)) {
      return;  // deliberately does not reply
    }
    MakeWellBehavedDevice(kDeviceMac)(request, mock);
  });
  RecordingObserver observer;
  auto config = FastConfig();
  config.keepalive_interval_millis = 0;
  config.keepalive_failure_threshold = 3;
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());

  // The first two failures should not be fatal.
  CHECK(machine.Poll().has_value());
  CHECK(observer.fatal_errors.empty());
  CHECK(machine.Poll().has_value());
  CHECK(observer.fatal_errors.empty());
  // The third reaches the threshold.
  const auto third = machine.Poll();
  CHECK_FALSE(third.has_value());
  REQUIRE(observer.fatal_errors.size() == 1);
  CHECK(observer.fatal_errors[0].find("保活") != std::string::npos);
}

TEST_CASE("软复位：AddressingReset 非零时必须重放包过滤") {
  // This is the easiest one to miss: without replay, data would never flow again after a reset.
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  channel.ClearSentMessages();
  REQUIRE(machine.Reset().has_value());

  // The reset was reported, with the addressing information marked as lost.
  REQUIRE(observer.reset_events.size() == 1);
  CHECK(observer.reset_events[0]);

  // RESET_MSG must have been sent.
  CHECK(channel.CountSent(MessageType::kReset) == 1);
  // And the packet filter must have been replayed.
  std::uint32_t filter = 0;
  REQUIRE_MESSAGE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter),
                  "复位后没有重放包过滤设置");
  CHECK(filter == kDefaultPacketFilter);
}

TEST_CASE("软复位：AddressingReset 为 0 时不必重放") {
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kReset)) {
      mock.EnqueueResponse(MakeResetComplete(/*addressing_reset=*/false));
      return;
    }
    MakeWellBehavedDevice(kDeviceMac)(request, mock);
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  channel.ClearSentMessages();
  REQUIRE(machine.Reset().has_value());
  REQUIRE(observer.reset_events.size() == 1);
  CHECK_FALSE(observer.reset_events[0]);

  std::uint32_t filter = 0;
  CHECK_FALSE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter));
}

TEST_CASE("停机：先清零包过滤再发 HALT") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  channel.ClearSentMessages();
  machine.Stop();
  CHECK(machine.CurrentState() == State::kUninitialized);

  // Must SET filter = 0 first (making the device fall back to initialized and stop sending data), then send HALT.
  std::uint32_t filter = 0xFFFF'FFFFU;
  REQUIRE(channel.FindSetUint32(Oid::kGenCurrentPacketFilter, filter));
  CHECK(filter == 0);
  CHECK(channel.CountSent(MessageType::kHalt) == 1);

  const std::size_t last = channel.SentMessages().size() - 1;
  CHECK(channel.SentMessageType(last) == ToRaw(MessageType::kHalt));
}

TEST_CASE("停机是幂等的") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  machine.Stop();
  const std::size_t after_first = channel.SentMessages().size();
  machine.Stop();
  CHECK(channel.SentMessages().size() == after_first);
}

TEST_CASE("Start 只能在未初始化状态下调用") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());
  CHECK_FALSE(machine.Start().has_value());
}

TEST_CASE("设备没有中断端点时退化为轮询，仍能完成启动") {
  // Linux's host driver ignores the interrupt endpoint entirely and relies purely on polling the control endpoint.
  MockControlChannel channel;
  channel.SetHasInterruptEndpoint(false);
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  const auto status = machine.Start();
  REQUIRE_MESSAGE(status.has_value(), Why(status));
  CHECK(machine.CurrentState() == State::kDataInitialized);
}

TEST_CASE("控制通道发送失败时启动失败且不卡在中间状态") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  channel.FailSendOnCall(1);  // the first INITIALIZE cannot even be sent
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  REQUIRE_FALSE(machine.Start().has_value());
  CHECK(machine.CurrentState() == State::kUninitialized);
}

TEST_CASE("设备 quirk：高通聚合补丁（MaxPacketsPerMessage=3、对齐因子 2）") {
  MockControlChannel channel;
  channel.SetRequestHandler(
      MakeWellBehavedDevice(kDeviceMac, /*max_transfer_size=*/3 * 1600,
                            /*max_packets=*/3, /*alignment_factor=*/2));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  CHECK(observer.parameters_snapshot.max_packets_per_message == 3);
  CHECK(observer.parameters_snapshot.tx_alignment_bytes == 4);  // 1 << 2
}

TEST_CASE("设备 quirk：对齐因子越界被钳到 7") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac, 2048, 1, /*alignment_factor=*/31));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());
  CHECK(observer.parameters_snapshot.tx_alignment_bytes == 128);
}

TEST_CASE("设备汇报的最大帧长小于协商 MTU 时下调 MTU") {
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    const std::uint32_t request_id = RequestIdOf(request);
    if (type == ToRaw(MessageType::kQuery) &&
        OidOf(request) == ToRaw(Oid::kGenMaximumFrameSize)) {
      mock.EnqueueResponse(MakeQueryCompleteUint32(request_id, 1400));
      return;
    }
    MakeWellBehavedDevice(kDeviceMac)(request, mock);
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());
  CHECK(machine.Parameters().mtu == 1400);
}

TEST_CASE("MillisUntilNextPoll 在保活周期内返回正值") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  auto config = FastConfig();
  config.keepalive_interval_millis = 5'000;
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());
  CHECK(machine.MillisUntilNextPoll() > 0);
  CHECK(machine.MillisUntilNextPoll() <= 5'000);
}

}  // TEST_SUITE("rndis.state_machine")

TEST_SUITE("rndis.state_machine_timeout") {

// * Regression cases for a class of hang defects that "the mock cannot catch" *
//
// The defect: Poll() had WaitForNotification(0) written in it, meaning "do not wait, just probe",
//       but on darwin libusb passes timeout to IOKit as both noDataTimeout and
//       completionTimeout, and **0 means wait forever** --
//       the control loop would hang forever on libusb_wait_for_event, unable even to respond to SIGTERM.
// The reason the mock cannot catch it is that it returns immediately for any timeout.
// So here we directly assert on the "timeout value passed down" itself.

TEST_CASE("绝不给 WaitForNotification 传 0（0 在 darwin 上是无限等待）") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  // Deliberately use a configuration that sets response_poll_interval_millis to 0 -- this is exactly the path where
  // min(control_timeout, interval*4) in Transact() computed 0 back then.
  auto config = FastConfig();
  config.response_poll_interval_millis = 0;
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());
  REQUIRE(machine.Poll().has_value());

  REQUIRE_FALSE(channel.NotificationTimeouts().empty());
  for (const std::uint32_t timeout : channel.NotificationTimeouts()) {
    CHECK_MESSAGE(timeout > 0,
                  "WaitForNotification 收到了 0 —— 在 darwin 上等于无限等待，会卡死控制线程");
  }
}

TEST_CASE("kProbeOnlyTimeoutMillis 本身必须非零") {
  CHECK(kProbeOnlyTimeoutMillis > 0);
}

}  // TEST_SUITE("rndis.state_machine_timeout")
