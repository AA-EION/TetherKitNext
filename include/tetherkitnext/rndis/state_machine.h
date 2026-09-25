// RNDIS 主机侧状态机。
//
// ★ 状态图（规范定义的是设备侧三态，主机侧镜像它并补上过渡态）★
//
//                      ┌──────────────────┐
//        ┌────────────►│  kUninitialized  │◄────────────┐
//        │             └────────┬─────────┘             │
//        │                      │ Start()               │ 收到 HALT 应答 /
//        │                      │ 发 INITIALIZE_MSG     │ 总线断开 / 致命错误
//        │                      ▼                       │
//        │             ┌──────────────────┐             │
//        │             │  kInitializing   ├─────────────┤
//        │             └────────┬─────────┘  协商失败   │
//        │                      │ 收 INITIALIZE_CMPLT   │
//        │                      │ 且协商通过            │
//        │                      ▼                       │
//        │             ┌──────────────────┐             │
//        │             │  kInitialized    │             │  ← 已可收发控制消息，
//        │             └────────┬─────────┘             │    但数据通道**未开**
//        │                      │ QUERY MAC/link 等     │
//        │                      │ SET packet filter≠0   │
//        │                      ▼                       │
//        │             ┌──────────────────┐             │
//        │        ┌────┤ kDataInitialized ├─────────────┤
//        │        │    └────────┬─────────┘             │
//        │        │             │ Stop()                │
//        │  SET filter=0       │ 发 HALT_MSG           │
//        │  （退回上一态）      ▼                       │
//        │             ┌──────────────────┐             │
//        └─────────────┤    kHalting      ├─────────────┘
//                      └──────────────────┘
//
//   规范的原文语义：
//     * 总线初始化后设备处于 RNDIS-uninitialized；
//     * 收到 INITIALIZE_MSG 并回 INITIALIZE_CMPLT(SUCCESS) → RNDIS-initialized；
//     * 收到 SET(OID_GEN_CURRENT_PACKET_FILTER, **非零**) → RNDIS-data-initialized，
//       **数据这才开始流动**；
//     * 在 data-initialized 下收到 SET(filter = 0) → 退回 RNDIS-initialized；
//     * 任意时刻收到 HALT_MSG 或总线断开 → 立即回 RNDIS-uninitialized。
//
// ★ 三条容易写错的协议细节 ★
//
//   1. **控制请求必须串行化。** GET_ENCAPSULATED_RESPONSE 只能「取下一条排队的
//      响应」，没有按 RequestId 选取的能力。所以同一时刻只能有一个请求在飞。
//
//   2. **设备会插队。** 在等某个 *_CMPLT 的过程中，设备完全可能先塞进来一条
//      REMOTE_NDIS_INDICATE_STATUS_MSG（媒体连接状态变化）或一条**设备发起的**
//      REMOTE_NDIS_KEEPALIVE_MSG（此时主机必须回 KEEPALIVE_CMPLT，否则设备可能
//      认为主机已死而断开）。因此控制读循环必须写成 dispatcher：按 MessageType
//      分派，不是「盲目假定读到的就是我要的那条」。
//
//   3. **RESET 没有 RequestId。** RESET_MSG 的 offset 8 是 Reserved，
//      RESET_CMPLT 的 offset 8 是 Status（不是 12）。所以 RESET 无法用 ID 配对，
//      同一时刻只能有一个在飞。而且 RESET_CMPLT 里的 AddressingReset 非零时，
//      主机**必须重发** SET(OID_GEN_CURRENT_PACKET_FILTER)（若设过组播还要重发
//      SET(OID_802_3_MULTICAST_LIST)）—— 否则复位后数据不会再流动。
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/common/time.h"
#include "tetherkitnext/rndis/messages.h"
#include "tetherkitnext/rndis/protocol.h"
#include "tetherkitnext/rndis/control_channel.h"

namespace tetherkitnext::rndis {

/// 主机侧状态。
enum class State : std::uint8_t {
  kUninitialized,     ///< 未初始化。控制通道可用，但设备还没同意开工。
  kInitializing,      ///< 已发 INITIALIZE_MSG，等 INITIALIZE_CMPLT。
  kInitialized,       ///< 协商完成。可读写 OID，但**数据通道还没开**。
  kDataInitialized,   ///< 已设非零包过滤，数据开始流动。
  kHalting,           ///< 已发 HALT_MSG，正在收尾。
};

[[nodiscard]] std::string_view StateName(State state) noexcept;

/// 设备汇报的、主机关心的设备信息。
struct DeviceInfo {
  /// 设备的永久 MAC。这是主机侧 feth 应该采用的地址 —— RNDIS 语义下设备就是
  /// 那块网卡，对端的 ARP 表与 DHCP 租约都按它建立。
  MacAddress permanent_address{};
  /// 当前 MAC。多数设备与 permanent 相同。
  MacAddress current_address{};
  bool has_permanent_address = false;
  bool has_current_address = false;

  /// 链路速率，**单位 100 bps**（这是 NDIS 的约定，不是 bps）。0 表示断连。
  std::uint32_t link_speed_100bps = 0;
  /// 最大帧长，**不含** 14 字节以太头。典型 1500。
  std::uint32_t maximum_frame_size = 0;
  /// 物理介质。可选 OID，查不到时保持 kUnspecified。
  PhysicalMedium physical_medium = PhysicalMedium::kUnspecified;
  /// 媒体连接状态。**注意 0 表示已连接。**
  MediaState media_state = MediaState::kConnected;
  /// 厂商描述串（不保证 NUL 结尾，这里已规整成 std::string）。
  std::string vendor_description;
  std::uint32_t vendor_id = 0;

  /// 链路速率换算成 Mbps，仅用于日志。
  [[nodiscard]] double LinkSpeedMbps() const noexcept {
    return static_cast<double>(link_speed_100bps) * 100.0 / 1e6;
  }
};

/// 状态机的配置。
struct StateMachineConfig {
  /// 希望使用的 MTU。设备装不下时会被 Negotiate 下调。
  std::uint32_t requested_mtu = kDefaultMtu;

  /// 主机在 INITIALIZE_MSG 里宣称的 MaxTransferSize，也是我们 bulk IN 缓冲的大小。
  ///
  /// **这是 RNDIS 吞吐最重要的杠杆**：它是让设备把多个 REMOTE_NDIS_PACKET_MSG
  /// 聚合进一次 bulk IN 的**唯一**手段。报得越大、设备聚合越多、每帧摊到的 USB
  /// 与系统调用开销越低。Linux 保守地只报 2048（一个满帧），我们报 16 KiB ——
  /// 既留足聚合空间，又不超过规范对 USB 1.1 设备的 0x4000 上限。
  std::uint32_t host_max_transfer_size = 16 * 1024;

  /// host → device 方向每次 bulk OUT 最多聚合几个 PACKET_MSG。0 表示不额外限制，
  /// 完全听设备汇报的 MaxPacketsPerMessage。
  ///
  /// 之所以留这个旋钮：设备汇报的聚合能力**未必可信**。主线 Linux gadget 的
  /// `rndis_rm_hdr()` 每次 bulk OUT 只拆一个 PACKET_MSG，而厂商内核照样可能汇报
  /// MaxPacketsPerMessage=10。若真遇上这种设备，除第一个以外的帧会被对端悄悄丢掉，
  /// 钳到 1 即可退回「一帧一传输」。
  ///
  /// ⚠️ **但别把它当成丢帧的默认解释。** 在实测过的 Android 设备上
  /// （汇报 10 包 / 15800 字节）做过 A/B：钳到 1 反而更慢（TCP TX 125 vs 234 Mbps），
  /// 丢帧率也没改善 —— 那台设备的聚合是**真的有效**的。所以这是一个诊断/兜底
  /// 旋钮，不是已知缺陷的开关。详见 docs/BENCHMARKS.md「真机端到端实测」。
  std::uint32_t max_tx_packets_per_message = 0;

  /// 要设置的包过滤位掩码。
  std::uint32_t packet_filter = kDefaultPacketFilter;

  /// 控制传输超时（毫秒）。
  std::uint32_t control_timeout_millis = kControlTimeoutMillis;

  /// 保活周期（毫秒）。
  ///
  /// 语义是「距上次从设备收到任何消息已过这么久」才发，而不是无条件定时发。
  /// 注意 Apple Silicon 上 libusb 的控制传输比 x64 慢约 10 倍（issue #1288），
  /// 所以别把这个值调得太小。
  std::uint32_t keepalive_interval_millis = kKeepAliveTimeoutMillis;

  /// 单次控制请求的响应轮询次数上限。
  ///
  /// Linux 的做法是最多轮询 10 次、每次间隔 40 ms（总约 400 ms）。我们先等中断
  /// 端点的通知，超时后再轮询，因此这里主要是给「没有中断端点」的设备兜底。
  std::uint32_t response_poll_attempts = 10;

  /// 每次轮询之间的等待（毫秒）。
  std::uint32_t response_poll_interval_millis = 40;

  /// 连续多少次保活失败后判定链路已死。
  std::uint32_t keepalive_failure_threshold = 3;
};

/// 状态机对外通报的事件。
///
/// 用回调而不是让调用方轮询状态，是因为「媒体连接状态变化」这类事件由设备主动
/// 推送（INDICATE_STATUS），轮询会漏掉边沿。
class StateMachineObserver {
 public:
  StateMachineObserver() = default;
  StateMachineObserver(const StateMachineObserver&) = delete;
  StateMachineObserver& operator=(const StateMachineObserver&) = delete;
  StateMachineObserver(StateMachineObserver&&) = delete;
  StateMachineObserver& operator=(StateMachineObserver&&) = delete;
  virtual ~StateMachineObserver() = default;

  /// 状态迁移。
  virtual void OnStateChanged(State from, State to) = 0;

  /// 协商完成，数据路径可以按这些参数搭建了。
  virtual void OnNegotiated(const NegotiatedParameters& parameters, const DeviceInfo& info) = 0;

  /// 链路 up / down（来自 INDICATE_STATUS 的 MEDIA_CONNECT / MEDIA_DISCONNECT，
  /// 或 QUERY OID_GEN_MEDIA_CONNECT_STATUS 的结果）。
  virtual void OnLinkStateChanged(bool connected) = 0;

  /// 设备复位完成，且要求主机重发寻址配置。数据路径需要短暂暂停。
  virtual void OnDeviceReset(bool addressing_lost) = 0;

  /// 链路已不可用（保活连续失败 / 设备断开 / 不可恢复的协议错误）。
  virtual void OnFatalError(const Error& error) = 0;
};

/// RNDIS 主机侧状态机。
///
/// **线程约束：本类的所有方法必须从同一个线程调用**（控制线程）。这不是实现
/// 偷懒，而是 libusb 的硬性要求 —— 同步控制传输在事件线程上会返回
/// LIBUSB_ERROR_BUSY，所以控制通道必须独占一个非事件线程。
class StateMachine {
 public:
  StateMachine(ControlChannel& channel, StateMachineObserver& observer,
               const StateMachineConfig& config);

  StateMachine(const StateMachine&) = delete;
  StateMachine& operator=(const StateMachine&) = delete;
  StateMachine(StateMachine&&) = delete;
  StateMachine& operator=(StateMachine&&) = delete;
  ~StateMachine() = default;

  [[nodiscard]] State CurrentState() const noexcept { return state_; }

  [[nodiscard]] const DeviceInfo& Info() const noexcept { return info_; }

  [[nodiscard]] const NegotiatedParameters& Parameters() const noexcept { return parameters_; }

  /// 走完完整的启动序列，直到 kDataInitialized。
  ///
  /// 序列（顺序照 Linux 的 generic_rndis_bind，逐步都有依据）：
  ///   1. INITIALIZE_MSG → INITIALIZE_CMPLT，协商 MaxTransferSize / 对齐 / MTU；
  ///   2. QUERY OID_GEN_PHYSICAL_MEDIUM（**可选 OID，失败不致命**）；
  ///   3. QUERY OID_802_3_PERMANENT_ADDRESS（拿主机侧要用的 MAC）；
  ///   4. QUERY OID_802_3_CURRENT_ADDRESS / OID_GEN_MAXIMUM_FRAME_SIZE /
  ///      OID_GEN_LINK_SPEED / OID_GEN_MEDIA_CONNECT_STATUS（均非致命）；
  ///   5. SET OID_GEN_CURRENT_PACKET_FILTER = 非零 → 数据开始流动。
  /// 任一致命步骤失败都会发 HALT_MSG 再返回错误。
  [[nodiscard]] Status Start();

  /// 优雅停机：SET filter = 0，再发 HALT_MSG。
  ///
  /// 即使中途出错也会尽最大努力把 HALT 发出去 —— 不发的话设备可能一直以为主机
  /// 还在，下次插上时状态不干净。
  void Stop();

  /// 处理一次「该做的周期性工作」。应由控制线程在循环里定期调用。
  ///
  /// 做两件事：
  ///   * 距上次从设备收到消息超过保活周期时，发一次 KEEPALIVE；
  ///   * 排空控制通道上设备主动推送的消息（INDICATE_STATUS / 设备发起的
  ///     KEEPALIVE），不要让它们堆积。
  ///
  /// 返回错误表示链路已不可用，调用方应重连。
  [[nodiscard]] Status Poll();

  /// 距下次该做周期性工作还剩多少毫秒。供调用方决定睡多久。
  [[nodiscard]] std::uint32_t MillisUntilNextPoll() const noexcept;

  /// 主动请求设备软复位。
  ///
  /// RESET 是 soft reset：控制通道保持完整，但设备会丢弃所有未完成的请求与数据包。
  /// 若 RESET_CMPLT 的 AddressingReset 非零，本方法会自动重发包过滤设置。
  [[nodiscard]] Status Reset();

 private:
  /// 一次控制请求-响应往返的结果。
  struct Exchange {
    /// 响应消息的字节视图，指向控制通道内部缓冲。
    std::span<const std::byte> response;
  };

  /// 发一条请求，然后等它的响应。
  ///
  /// 期间会正确处理设备的插队消息（INDICATE_STATUS / 设备发起的 KEEPALIVE）：
  /// 分派掉它们并继续等我们要的那条。
  [[nodiscard]] Result<Exchange> Transact(std::span<const std::byte> request,
                                          MessageType expected_reply);

  /// 读取并分派一条设备推送的消息。
  ///
  /// @param expected_reply 我们正在等的消息类型；读到它就返回 true 并把视图
  ///                       写进 out_response。传 std::nullopt 表示「只排空推送
  ///                       消息，不等任何特定回复」。
  [[nodiscard]] Result<bool> PumpOnce(std::optional<MessageType> expected_reply,
                                      std::span<const std::byte>& out_response);

  /// 处理一条 INDICATE_STATUS。
  void HandleIndicateStatus(std::span<const std::byte> message);

  /// 处理设备主动发来的 KEEPALIVE_MSG —— **必须回 KEEPALIVE_CMPLT**。
  [[nodiscard]] Status HandleDeviceKeepAlive(std::span<const std::byte> message);

  /// 查一个 OID。`fatal` 为 false 时，设备返回不支持不算错误。
  [[nodiscard]] Result<std::span<const std::byte>> QueryOid(Oid oid,
                                                            std::uint32_t expected_bytes,
                                                            bool fatal);

  /// 写一个 LE32 型 OID。
  [[nodiscard]] Status SetOidUint32(Oid oid, std::uint32_t value);

  /// 采集设备信息（第 2~4 步）。非致命 OID 的失败只记日志。
  [[nodiscard]] Status CollectDeviceInfo();

  /// 发 HALT_MSG。尽最大努力，不返回错误。
  void SendHalt() noexcept;

  void TransitionTo(State next);

  /// 分配下一个 RequestId。
  ///
  /// 从 1 开始递增并跳过 0：某些设备把 0 当作「无效 ID」。
  [[nodiscard]] std::uint32_t NextRequestId() noexcept;

  /// 记录「刚从设备收到了消息」，用于保活的空闲判定。
  void MarkDeviceActivity() noexcept { last_device_activity_ = MonotonicNanos(); }

  ControlChannel* channel_;
  StateMachineObserver* observer_;
  StateMachineConfig config_;

  State state_ = State::kUninitialized;
  DeviceInfo info_;
  NegotiatedParameters parameters_;

  std::uint32_t next_request_id_ = 1;
  Nanos last_device_activity_ = 0;
  Nanos next_keepalive_deadline_ = 0;
  std::uint32_t consecutive_keepalive_failures_ = 0;

  /// 上一次成功设置的包过滤值。RESET 后要求重放时用它。
  std::uint32_t active_packet_filter_ = 0;

  /// 请求消息的组装缓冲。控制消息很小，一个缓冲足够。
  std::vector<std::byte> request_buffer_;

  bool link_connected_ = false;
};

}  // namespace tetherkitnext::rndis
