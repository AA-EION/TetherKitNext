// RNDIS 状态机的单元测试。
//
// 状态机是整个协议实现里逻辑最复杂的部分，而它在真机上的很多路径极难复现
// （设备插队推送、保活失败、复位后要求重放）。这里用 MockControlChannel 扮演
// 设备侧，把这些路径全部覆盖到。
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest.h>

#include "language_guard.h"

#include "mock_control_channel.h"
#include "tetherkitnext/rndis/state_machine.h"

// 与 test_net_link.cc 里同名助手同理：直接在 REQUIRE_MESSAGE 里写
// `r ? "" : r.error().ToString()` 编译不过 —— doctest 的 MessageBuilder 会把
// 整个三元表达式吞进流里再试图转 bool。
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

/// 测试用配置：把轮询间隔压到 0，让测试跑得快。
[[nodiscard]] StateMachineConfig FastConfig() {
  StateMachineConfig config;
  config.response_poll_interval_millis = 0;
  config.response_poll_attempts = 4;
  config.keepalive_interval_millis = 0;  // 让保活立即到期，便于测试
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

  // 状态迁移路径必须是 未初始化 → 初始化中 → 已初始化 → 数据已就绪。
  REQUIRE(observer.transitions.size() == 3);
  CHECK(observer.transitions[0].to == State::kInitializing);
  CHECK(observer.transitions[1].to == State::kInitialized);
  CHECK(observer.transitions[2].to == State::kDataInitialized);

  CHECK(observer.negotiated_count == 1);
  CHECK(observer.parameters_snapshot.mtu == 1500);
  CHECK(observer.parameters_snapshot.device_max_transfer_size == 2048);
  CHECK(observer.parameters_snapshot.max_packets_per_message == 1);
  CHECK(observer.parameters_snapshot.tx_alignment_bytes == 1);

  // 拿到了设备 MAC —— 这是主机侧 feth 要用的地址。
  CHECK(machine.Info().has_permanent_address);
  CHECK(machine.Info().permanent_address == kDeviceMac);
  CHECK(machine.Info().LinkSpeedMbps() == doctest::Approx(480.0));

  // 链路通报了一次「已连接」。
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
  // 第一条必须是 INITIALIZE_MSG。
  CHECK(channel.SentMessageType(0) == ToRaw(MessageType::kInitialize));
  // 最后一条必须是 SET（包过滤）—— 它才让设备进入 data-initialized。
  const std::size_t last = channel.SentMessages().size() - 1;
  CHECK(channel.SentMessageType(last) == ToRaw(MessageType::kSet));

  // 只发了一条 INITIALIZE，没有重复。
  CHECK(channel.CountSent(MessageType::kInitialize) == 1);

  // 设置的包过滤值必须是我们要求的那个（DIRECTED|BROADCAST|ALL_MULTICAST|PROMISCUOUS）。
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
      // 变长 OID 传非零长度会被 ActiveSync 实现拒绝。
      CHECK_MESSAGE(length == 0, "变长 OID " << OidName(oid) << " 的长度应为 0");
      saw_variable = true;
    } else {
      CHECK_MESSAGE(length > 0, "定长 OID " << OidName(oid) << " 的长度应大于 0");
      saw_fixed = true;
    }
  }
  CHECK(saw_fixed);
  CHECK(saw_variable);  // 启动序列里查了 OID_GEN_VENDOR_DESCRIPTION
}

TEST_CASE("可选 OID 返回 NOT_SUPPORTED 不影响启动") {
  // OID_GEN_PHYSICAL_MEDIUM 是可选的，设备回不支持是完全正常的。
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
      // 所有 QUERY 都回不支持 —— 包括永久 MAC。
      mock.EnqueueResponse(
          MakeQueryComplete(request_id, {}, ToRaw(StatusCode::kNotSupported)));
    }
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());

  const auto status = machine.Start();
  REQUIRE_FALSE(status.has_value());
  // 失败后必须回到未初始化，而不是卡在中间状态。
  CHECK(machine.CurrentState() == State::kUninitialized);
  // 且必须发过 HALT 来清理设备侧状态。
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
  // 这是真机上很常见但极难主动复现的路径。
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kInitialize)) {
      // 先塞一条媒体断开通报，再塞真正的响应。
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

  // 插队消息必须被消费掉，不能堆在队列里挤占后续响应。
  CHECK(channel.PendingResponseCount() == 0);

  // 关于链路事件：链路状态是**边沿触发**的。启动时内部认定的初始状态就是
  // 「未连接」，所以一条 MEDIA_DISCONNECT 与当前认知一致 → 正确地不产生事件
  // （去重）。随后 QUERY OID_GEN_MEDIA_CONNECT_STATUS 返回 0（已连接），
  // 这才是更新的、权威的状态，于是 Start() 结束时通报一次「已连接」。
  REQUIRE(observer.link_events.size() == 1);
  CHECK(observer.link_events[0]);
}

TEST_CASE("链路已 up 之后收到 MEDIA_DISCONNECT 才产生断开事件") {
  // 这是 MEDIA_DISCONNECT 真正该触发事件的场景：存在 up → down 的边沿。
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  auto config = FastConfig();
  config.keepalive_interval_millis = 60'000;  // 别让保活干扰
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());

  // 启动后链路是 up 的。
  REQUIRE(observer.link_events.size() == 1);
  REQUIRE(observer.link_events[0]);

  // 设备现在推一条 MEDIA_DISCONNECT，Poll 应把它取走并通报断开。
  channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
  REQUIRE(machine.Poll().has_value());

  REQUIRE(observer.link_events.size() == 2);
  CHECK_FALSE(observer.link_events[1]);

  // 再推一条相同的通报 —— 边沿触发，不该再产生事件。
  channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaDisconnect));
  REQUIRE(machine.Poll().has_value());
  CHECK(observer.link_events.size() == 2);

  // 恢复连接时应产生 down → up 的边沿。
  channel.EnqueueResponse(MakeIndicateStatus(StatusCode::kMediaConnect));
  REQUIRE(machine.Poll().has_value());
  REQUIRE(observer.link_events.size() == 3);
  CHECK(observer.link_events[2]);
}

TEST_CASE("设备主动发 KEEPALIVE_MSG 时主机必须回 KEEPALIVE_CMPLT") {
  // 不回的话设备可能判定主机已死而断开连接。
  MockControlChannel channel;
  constexpr std::uint32_t kDeviceKeepAliveId = 0xABCD'1234;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kInitialize)) {
      // 在响应之前插入一条设备发起的保活。
      mock.EnqueueResponse(MakeDeviceKeepAlive(kDeviceKeepAliveId));
      mock.EnqueueResponse(MakeInitializeComplete(RequestIdOf(request)));
      return;
    }
    MakeWellBehavedDevice(kDeviceMac)(request, mock);
  });
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  // 主机必须发过一条 KEEPALIVE_CMPLT，且 RequestId 是设备给的那个。
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
  // 下面断言错误消息的中文措辞，先把语言钉死。
  const tetherkitnext::testing::ScopedLanguage guard{tetherkitnext::Language::kChinese};
  MockControlChannel channel;
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    if (LoadLe32(request.data() + kMessageTypeOffset) == ToRaw(MessageType::kInitialize)) {
      // 塞一条 CONDIS 消息。
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
  // 错误信息必须明确指出是面向连接设备，而不是含糊的「未知消息」。
  CHECK(status.error().ToString().find("面向连接") != std::string::npos);
}

TEST_CASE("保活正常时不报错，且设备活跃期间不发无谓保活") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  auto config = FastConfig();
  // 保活周期设很大：Poll 不该发保活。
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
  config.keepalive_interval_millis = 0;  // 立即到期
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());

  channel.ClearSentMessages();
  REQUIRE(machine.Poll().has_value());
  CHECK(channel.CountSent(MessageType::kKeepAlive) == 1);
  CHECK(observer.fatal_errors.empty());
}

TEST_CASE("连续保活失败达到阈值后通报致命错误") {
  // 下面断言致命错误里的中文措辞，先把语言钉死。
  const tetherkitnext::testing::ScopedLanguage guard{tetherkitnext::Language::kChinese};
  MockControlChannel channel;
  // 设备只回 INITIALIZE / QUERY / SET，对 KEEPALIVE 一律不回 → 每次都超时。
  channel.SetRequestHandler([](std::span<const std::byte> request, MockControlChannel& mock) {
    const std::uint32_t type = LoadLe32(request.data() + kMessageTypeOffset);
    if (type == ToRaw(MessageType::kKeepAlive)) {
      return;  // 故意不回
    }
    MakeWellBehavedDevice(kDeviceMac)(request, mock);
  });
  RecordingObserver observer;
  auto config = FastConfig();
  config.keepalive_interval_millis = 0;
  config.keepalive_failure_threshold = 3;
  StateMachine machine(channel, observer, config);
  REQUIRE(machine.Start().has_value());

  // 前两次失败不该致命。
  CHECK(machine.Poll().has_value());
  CHECK(observer.fatal_errors.empty());
  CHECK(machine.Poll().has_value());
  CHECK(observer.fatal_errors.empty());
  // 第三次达到阈值。
  const auto third = machine.Poll();
  CHECK_FALSE(third.has_value());
  REQUIRE(observer.fatal_errors.size() == 1);
  CHECK(observer.fatal_errors[0].find("保活") != std::string::npos);
}

TEST_CASE("软复位：AddressingReset 非零时必须重放包过滤") {
  // 这是最容易漏的一条：不重放的话复位后数据就再也不流动了。
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  StateMachine machine(channel, observer, FastConfig());
  REQUIRE(machine.Start().has_value());

  channel.ClearSentMessages();
  REQUIRE(machine.Reset().has_value());

  // 通报了复位，且标明寻址信息已丢失。
  REQUIRE(observer.reset_events.size() == 1);
  CHECK(observer.reset_events[0]);

  // 必须发过 RESET_MSG。
  CHECK(channel.CountSent(MessageType::kReset) == 1);
  // 而且必须重放了包过滤。
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

  // 必须先 SET filter = 0（让设备退回 initialized、停止发数据），再发 HALT。
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
  // Linux 的 host 驱动就完全忽略中断端点，纯靠轮询控制端点。
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
  channel.FailSendOnCall(1);  // 第一条 INITIALIZE 就发不出去
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

// ★ 针对一类「mock 测不出来」的挂死缺陷的回归用例 ★
//
// 缺陷：Poll() 里写了 WaitForNotification(0)，意图是「不等待、只探一下」，
//       但 libusb 在 darwin 上把 timeout 同时作为 noDataTimeout 与
//       completionTimeout 传给 IOKit，**0 表示无限等待** ——
//       控制循环会永久卡死在 libusb_wait_for_event 上，连 SIGTERM 都响应不了。
// mock 之所以测不出来，是因为它对任何超时都立即返回。
// 所以这里改为直接断言「传下去的超时值」本身。

TEST_CASE("绝不给 WaitForNotification 传 0（0 在 darwin 上是无限等待）") {
  MockControlChannel channel;
  channel.SetRequestHandler(MakeWellBehavedDevice(kDeviceMac));
  RecordingObserver observer;
  // 刻意用把 response_poll_interval_millis 设成 0 的配置 —— 这正是当初
  // Transact() 里 min(control_timeout, interval*4) 算出 0 的那条路径。
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
