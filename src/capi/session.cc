// 会话的 C ABI：生命周期、状态快照、事件轮询。
//
// 这一层几乎没有逻辑，只做三件事：POD 配置 → RuntimeConfig、RuntimeSnapshot →
// POD 状态、RuntimeEvent → 环形队列。真正的编排全在 core::Runtime 里。
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>
#include <utility>

#include "capi_support.h"
#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/time.h"
#include "tetherkitnext/core/runtime.h"
#include "tetherkitnext/net/feth_device.h"
#include "tetherkitnext/rndis/state_machine.h"

namespace {

using tetherkitnext::capi::ClearError;
using tetherkitnext::capi::CopyText;
using tetherkitnext::capi::FillError;
using tetherkitnext::capi::WallNanos;
using tetherkitnext::core::RunState;
using tetherkitnext::core::RuntimeEvent;

// ---------------------------------------------------------------------------
// 枚举值对齐检查
//
// C 侧的枚举是照 C++ 侧抄的数值，两边靠「顺序恰好一致」直接强转。这种耦合一旦
// 有人在中间插入一个枚举值就会静默错位（界面显示错状态，且没有任何报错），
// 所以在编译期把它焊死。
// ---------------------------------------------------------------------------
static_assert(static_cast<int>(RunState::kIdle) == TK_RUN_IDLE);
static_assert(static_cast<int>(RunState::kStarting) == TK_RUN_STARTING);
static_assert(static_cast<int>(RunState::kRunning) == TK_RUN_RUNNING);
static_assert(static_cast<int>(RunState::kStopping) == TK_RUN_STOPPING);
static_assert(static_cast<int>(RunState::kStopped) == TK_RUN_STOPPED);
static_assert(static_cast<int>(RunState::kFailed) == TK_RUN_FAILED);

static_assert(static_cast<int>(tetherkitnext::rndis::State::kUninitialized) == TK_RNDIS_UNINITIALIZED);
static_assert(static_cast<int>(tetherkitnext::rndis::State::kInitializing) == TK_RNDIS_INITIALIZING);
static_assert(static_cast<int>(tetherkitnext::rndis::State::kInitialized) == TK_RNDIS_INITIALIZED);
static_assert(static_cast<int>(tetherkitnext::rndis::State::kDataInitialized) ==
              TK_RNDIS_DATA_INITIALIZED);
static_assert(static_cast<int>(tetherkitnext::rndis::State::kHalting) == TK_RNDIS_HALTING);

static_assert(static_cast<int>(RuntimeEvent::Kind::kRndisState) == TK_EVENT_RNDIS_STATE);
static_assert(static_cast<int>(RuntimeEvent::Kind::kNegotiated) == TK_EVENT_NEGOTIATED);
static_assert(static_cast<int>(RuntimeEvent::Kind::kLink) == TK_EVENT_LINK);
static_assert(static_cast<int>(RuntimeEvent::Kind::kDeviceReset) == TK_EVENT_DEVICE_RESET);
static_assert(static_cast<int>(RuntimeEvent::Kind::kFatal) == TK_EVENT_FATAL);
static_assert(static_cast<int>(RuntimeEvent::Kind::kRunState) == TK_EVENT_RUN_STATE);

/// 事件环形缓冲。
///
/// 容量 128 的依据：事件只在状态迁移时产生，一次完整的启动序列约 10 条。128
/// 足以覆盖「设备反复插拔 + 链路抖动」这类连续事件，而 GUI 每 500 ms 就会取空。
/// 满了丢最旧 —— 事件只用于做动画和提示，权威状态在快照里，漏几条不影响正确性。
constexpr std::size_t kEventRingCapacity = 128;

class EventRing final : public tetherkitnext::core::RuntimeEventSink {
 public:
  /// 由控制线程调用。
  void OnRuntimeEvent(const RuntimeEvent& event) noexcept override {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == kEventRingCapacity) {
      head_ = (head_ + 1) % kEventRingCapacity;
      --count_;
    }
    tk_event_t& slot = events_[(head_ + count_) % kEventRingCapacity];
    slot.kind = static_cast<std::int32_t>(event.kind);
    slot.wall_nanos = WallNanos();
    slot.a = event.a;
    slot.b = event.b;
    CopyText(slot.text, event.text);
    ++count_;
  }

  /// 由宿主线程调用。
  std::size_t Drain(tk_event_t* out_events, std::size_t capacity) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::size_t taken = 0;
    while (taken < capacity && count_ > 0) {
      if (out_events != nullptr) {
        out_events[taken] = events_[head_];
      }
      head_ = (head_ + 1) % kEventRingCapacity;
      --count_;
      ++taken;
    }
    return taken;
  }

 private:
  std::mutex mutex_;
  std::array<tk_event_t, kEventRingCapacity> events_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

/// 取值，0 视作「用库的默认值」。
///
/// 对调优旋钮宽容而不是报错：GUI 可能只关心 MTU，把其余字段留空，硬要求它们
/// 非零只会逼调用方复制一遍默认值 —— 而复制出来的常量迟早和库里的不同步。
[[nodiscard]] std::uint32_t OrDefault(std::uint32_t value, std::uint32_t fallback) noexcept {
  return value != 0 ? value : fallback;
}

[[nodiscard]] tetherkitnext::core::RuntimeConfig ToRuntimeConfig(const tk_session_config_t& config,
                                                             EventRing& sink) {
  const tetherkitnext::core::RuntimeConfig defaults;
  tetherkitnext::core::RuntimeConfig result;

  result.device_filter.vendor_id = config.vendor_id;
  result.device_filter.product_id = config.product_id;
  result.device_filter.bus_number = config.bus_number;
  result.device_filter.device_address = config.device_address;

  result.mtu = OrDefault(config.mtu, defaults.mtu);
  result.adopt_device_mac = config.adopt_device_mac;

  result.data_channel.rx_transfer_count =
      OrDefault(config.rx_transfer_count, defaults.data_channel.rx_transfer_count);
  result.data_channel.tx_transfer_count =
      OrDefault(config.tx_transfer_count, defaults.data_channel.tx_transfer_count);
  result.data_channel.rx_transfer_bytes =
      OrDefault(config.rx_transfer_kib * 1024, defaults.data_channel.rx_transfer_bytes);
  result.rndis.host_max_transfer_size =
      OrDefault(config.max_transfer_kib * 1024, defaults.rndis.host_max_transfer_size);
  result.bpf.kernel_buffer_bytes =
      OrDefault(config.bpf_buffer_kib * 1024, defaults.bpf.kernel_buffer_bytes);

  result.event_sink = &sink;
  return result;
}

}  // namespace

// 会话对象。
//
// ★ 成员声明顺序即销毁顺序的倒序，这里不能乱调 ★
//   events 必须声明在 runtime **之前**，这样销毁时 runtime 先走 —— 它的析构
//   会 join 控制线程，而控制线程直到最后一刻都可能往 events 里塞事件。
//   反过来就是 use-after-free。
struct tk_session {
  EventRing events;
  std::unique_ptr<tetherkitnext::core::Runtime> runtime;
};

void tk_session_config_init(tk_session_config_t* out_config) {
  if (out_config == nullptr) {
    return;
  }
  const tetherkitnext::core::RuntimeConfig defaults;
  *out_config = tk_session_config_t{};
  out_config->mtu = defaults.mtu;
  out_config->adopt_device_mac = defaults.adopt_device_mac;
  out_config->rx_transfer_count = defaults.data_channel.rx_transfer_count;
  out_config->tx_transfer_count = defaults.data_channel.tx_transfer_count;
  out_config->rx_transfer_kib = defaults.data_channel.rx_transfer_bytes / 1024;
  out_config->max_transfer_kib = defaults.rndis.host_max_transfer_size / 1024;
  out_config->bpf_buffer_kib = defaults.bpf.kernel_buffer_bytes / 1024;
}

tk_session_t* tk_session_create(const tk_session_config_t* config, tk_error_t* out_error) {
  ClearError(out_error);
  if (config == nullptr) {
    tetherkitnext::capi::FillGenericError(out_error,
                                      tetherkitnext::Tr(tetherkitnext::Msg::kCapiSessionConfigNull));
    return nullptr;
  }

  // 会话是唯一会创建 feth 的入口，登记回调在这里装上就够 —— 免 root 的那组
  // 接口（版本、枚举、预检）因此保持零副作用，不会去碰 /var/run。
  tetherkitnext::capi::InstallInterfaceRegistry();

  auto session = std::unique_ptr<tk_session>(new (std::nothrow) tk_session());
  if (session == nullptr) {
    tetherkitnext::capi::FillGenericError(out_error,
                                      tetherkitnext::Tr(tetherkitnext::Msg::kCapiSessionOutOfMemory));
    return nullptr;
  }

  auto runtime = tetherkitnext::core::Runtime::Create(ToRuntimeConfig(*config, session->events));
  if (!runtime) {
    FillError(out_error, runtime.error());
    return nullptr;
  }
  session->runtime = std::move(*runtime);
  return session.release();
}

tk_result_t tk_session_start(tk_session_t* session, tk_error_t* out_error) {
  ClearError(out_error);
  if (session == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }

  if (const auto status = session->runtime->Start(); !status) {
    FillError(out_error, status.error());
    // 唯一会同步失败的是 root 检查（见 Runtime::Start），单独给个专门的码，
    // GUI 好据此弹「需要授权」而不是笼统的「启动失败」。
    return tetherkitnext::net::IsRunningAsRoot() ? TK_ERR_FAILED : TK_ERR_PERMISSION;
  }
  return TK_OK;
}

tk_result_t tk_session_stop(tk_session_t* session) {
  if (session == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }
  session->runtime->Stop();
  return TK_OK;
}

void tk_session_destroy(tk_session_t* session) {
  // Runtime 的析构会 Stop()（幂等），所以这里直接 delete 就够。
  delete session;  // NOLINT(cppcoreguidelines-owning-memory)
}

tk_result_t tk_session_status_get(tk_session_t* session, tk_session_status_t* out_status) {
  if (session == nullptr || out_status == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }

  const tetherkitnext::core::RuntimeSnapshot snapshot = session->runtime->Snapshot();
  *out_status = tk_session_status_t{};

  out_status->run_state = static_cast<std::int32_t>(snapshot.run_state);
  out_status->rndis_state = static_cast<std::int32_t>(snapshot.rndis_state);
  out_status->link_up = snapshot.link_up;
  out_status->paused = snapshot.paused;

  CopyText(out_status->system_interface, snapshot.system_interface);
  CopyText(out_status->driver_interface, snapshot.driver_interface);
  CopyText(out_status->vendor_description, snapshot.device_info.vendor_description);
  CopyText(out_status->device_description, snapshot.device_description);

  // 用 permanent 而非 current：RNDIS 语义下设备就是这块网卡，对端的 ARP 表与
  // DHCP 租约都按永久地址建立，界面上显示它才对得上用户在路由器里看到的条目。
  static_assert(sizeof(out_status->device_mac) ==
                std::tuple_size_v<tetherkitnext::rndis::MacAddress>);
  std::ranges::copy(snapshot.device_info.permanent_address, std::begin(out_status->device_mac));

  out_status->mtu = snapshot.parameters.mtu;
  out_status->link_speed_mbps = static_cast<std::uint32_t>(snapshot.device_info.LinkSpeedMbps());

  out_status->rx_frames = snapshot.bridge.rx.frames;
  out_status->rx_bytes = snapshot.bridge.rx.bytes;
  out_status->rx_dropped = snapshot.bridge.rx.TotalDropped();
  out_status->tx_frames = snapshot.bridge.tx.frames;
  out_status->tx_bytes = snapshot.bridge.tx.bytes;
  out_status->tx_dropped = snapshot.bridge.tx.TotalDropped();
  out_status->rx_queue_depth = snapshot.bridge.rx_queue_depth;
  out_status->link_kernel_drops = snapshot.bridge.link_kernel_drops;
  out_status->tx_backpressure = snapshot.bridge.tx_backpressure_events;

  // 用库这边的单调时钟给快照打时间戳，而不是让宿主用自己的时钟做差：
  // 两次拉取之间的真实间隔会被调度拉长，用宿主的定时器周期当分母会把速率算高。
  out_status->monotonic_nanos = static_cast<std::int64_t>(tetherkitnext::MonotonicNanos());

  CopyText(out_status->fatal, snapshot.fatal_message);
  return TK_OK;
}

size_t tk_session_poll_events(tk_session_t* session, tk_event_t* out_events, size_t capacity) {
  if (session == nullptr || capacity == 0) {
    return 0;
  }
  return session->events.Drain(out_events, capacity);
}
