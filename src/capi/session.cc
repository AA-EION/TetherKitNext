// The session's C ABI: lifecycle, state snapshots, event polling.
//
// This layer has almost no logic and does only three things: POD configuration -> RuntimeConfig, RuntimeSnapshot ->
// POD status, RuntimeEvent -> ring queue. The real orchestration is all in core::Runtime.
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
// Enum value alignment check
//
// The C-side enums are values copied from the C++ side, and the two sides are directly cast to each other relying on "the order happening to be the same". This coupling
// silently misaligns as soon as someone inserts an enum value in the middle (the UI shows the wrong state, with no error whatsoever),
// so it is welded shut at compile time.
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

/// Event ring buffer.
///
/// Basis for capacity 128: events are produced only at state transitions, and a full startup sequence is about 10. 128
/// is enough to cover continuous events such as "the device being repeatedly plugged and unplugged + link flapping", and the GUI drains it every 500 ms.
/// When full, drop the oldest -- events are only used for animation and hints, the authoritative state is in the snapshot, and missing a few does not affect correctness.
constexpr std::size_t kEventRingCapacity = 128;

class EventRing final : public tetherkitnext::core::RuntimeEventSink {
 public:
  /// Called by the control thread.
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

  /// Called by the host thread.
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

/// Takes a value; 0 is treated as "use the library default".
///
/// Be lenient with tuning knobs rather than reporting an error: the GUI may care only about the MTU and leave the other fields empty, and insisting they
/// be non-zero would only force callers to copy the defaults -- and the copied constants would sooner or later drift out of sync with the library's.
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

// The session object.
//
// * Member declaration order is the reverse of destruction order; it must not be shuffled here *
//   events must be declared **before** runtime, so that on destruction runtime goes first -- its destructor
//   joins the control thread, and the control thread may push events into events up to the last moment.
//   The reverse is a use-after-free.
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

  // The session is the only entry point that creates a feth, so installing the registration callback here is enough -- the root-free group of
  // interfaces (version, enumeration, preflight) therefore stays free of side effects and never touches /var/run.
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
    // The only thing that fails synchronously is the root check (see Runtime::Start); give it a dedicated code,
    // so the GUI can pop up "authorization needed" instead of a generic "startup failed".
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
  // Runtime's destructor calls Stop() (idempotent), so a direct delete is enough here.
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

  // permanent is used rather than current: under RNDIS semantics the device is this NIC, and the peer's ARP table and
  // DHCP lease are both built on the permanent address, so showing it in the UI is what matches the entries the user sees in the router.
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

  // The library's own monotonic clock is used to timestamp the snapshot, rather than having the host difference its own clock:
  // the real interval between two pulls gets stretched by scheduling, and using the host's timer period as the denominator would overestimate the rate.
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
