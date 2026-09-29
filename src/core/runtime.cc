#include "tetherkitnext/core/runtime.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/common/scheduling.h"
#include "tetherkitnext/common/time.h"

namespace tetherkitnext::core {

std::string_view RunStateName(RunState state) noexcept {
  switch (state) {
    case RunState::kIdle:
      return Text(Msg::kCoreRunStateIdle);
    case RunState::kStarting:
      return Text(Msg::kCoreRunStateStarting);
    case RunState::kRunning:
      return Text(Msg::kCoreRunStateRunning);
    case RunState::kStopping:
      return Text(Msg::kCoreRunStateStopping);
    case RunState::kStopped:
      return Text(Msg::kCoreRunStateStopped);
    case RunState::kFailed:
      return Text(Msg::kCoreRunStateFailed);
  }
  return Text(Msg::kCoreRunStateUnknown);
}

Result<std::unique_ptr<Runtime>> Runtime::Create(const RuntimeConfig& config) {
  auto runtime = std::unique_ptr<Runtime>(new Runtime());
  runtime->config_ = config;
  return runtime;
}

Runtime::~Runtime() {
  Stop();
}

// =============================================================================
// Host thread interface
// =============================================================================

Status Runtime::Start() {
  if (started_) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreRuntimeAlreadyStarted)));
  }

  // The root check is deliberately kept on the **synchronous** path.
  //
  // It needs no I/O, and "forgot sudo" is the most common failure; reporting it synchronously and early lets the command line
  // reflect it in the return code, and spares the user from staring at a spinning UI waiting for an asynchronous event.
  // Note that libusb claiming the RNDIS interface **does not need** root (macOS has no RNDIS kernel driver),
  // what needs root is feth creation and opening /dev/bpf*.
  if (!net::IsRunningAsRoot()) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreNeedsRoot)));
  }

  started_ = true;
  SetRunState(RunState::kStarting);
  control_thread_ = std::thread([this] { RunControlThread(); });
  return Ok();
}

void Runtime::WaitUntilStopped() {
  if (control_thread_.joinable()) {
    control_thread_.join();
  }
}

void Runtime::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  if (!started_) {
    return;
  }

  // Set the shutdown flag and wake the control thread that may be sleeping.
  //
  // The flag must be written while holding the lock: the control thread's wait_for uses it as the predicate, and writing without the lock leaves a
  // lost-wakeup window of "checked the predicate, not yet asleep". RequestStop() not taking the lock is a deliberate
  // trade-off (it must stay async-signal-safe), at the cost of being seen up to one loop period later.
  {
    const std::lock_guard<std::mutex> guard(stop_mutex_);
    stop_requested_.store(true, std::memory_order_release);
  }
  stop_condition_.notify_all();

  if (control_thread_.joinable()) {
    control_thread_.join();
  }
}

RuntimeSnapshot Runtime::Snapshot() const {
  const std::lock_guard<std::mutex> guard(snapshot_mutex_);
  return snapshot_;
}

std::string Runtime::SystemInterfaceName() const {
  const std::lock_guard<std::mutex> guard(snapshot_mutex_);
  return snapshot_.system_interface;
}

// =============================================================================
// Control thread
// =============================================================================

void Runtime::RunControlThread() noexcept {
  // The control thread uses the kControl QoS rather than kDataPath: it does a bit of work only every few seconds,
  // and competing for performance cores is meaningless for it, and might instead crowd out the three real data-path threads.
  ConfigureCurrentThread("rndis-ctl", ThreadRole::kControl);

  if (const auto status = RunStartSequence(); !status) {
    RecordFatal(status.error());
  } else {
    SetRunState(RunState::kRunning);
    RunControlLoop();
    SetRunState(RunState::kStopping);
  }

  // Tear down whether startup succeeded or not: the startup sequence may have built half of things already (for example the feth was built
  // but BPF could not be opened), and missing this would leave the NIC in the kernel.
  Teardown();

  SetRunState(fatal_error_.load(std::memory_order_acquire) ? RunState::kFailed
                                                           : RunState::kStopped);
}

Status Runtime::RunStartSequence() {
  // ---- Step 1: libusb context and event thread ----
  //
  // (The root check was already done synchronously in Start().)
  TETHERKITNEXT_ASSIGN_OR_RETURN(usb_context_, usb::Context::Create());

  // ---- Step 2: discover and open the device ----
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto candidates,
                             usb::FindRndisDevices(*usb_context_, config_.device_filter));
  if (candidates.empty()) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreNoDeviceFound)));
  }
  if (candidates.size() > 1) {
    TETHERKITNEXT_INFO_TR(Msg::kCoreMultipleDevices, candidates.size());
  }
  TETHERKITNEXT_ASSIGN_OR_RETURN(device_, usb::Device::Open(*usb_context_, candidates.front()));
  RefreshSnapshot();

  // ---- Step 3: RNDIS control channel and state machine ----
  //
  // The control channel uses the synchronous libusb API, so it must run on a **non-event thread** -- this function is on
  // Runtime's own control thread, which satisfies the requirement.
  control_channel_ =
      std::make_unique<usb::UsbControlChannel>(*device_, config_.rndis.control_timeout_millis);

  // Interrupt notifications must go through an asynchronous transfer, otherwise the control thread would hang forever -- see
  // the explanation of UsbControlChannel in include/tetherkitnext/usb/device.h.
  // It must be started **before** the state machine Start(), because the initialization handshake itself waits for a notification.
  TETHERKITNEXT_RETURN_IF_ERROR(control_channel_->StartNotificationListener());

  // Let RNDIS negotiation use the real endpoint max packet size (which affects the derivation of MaxTransferSize).
  rndis::StateMachineConfig rndis_config = config_.rndis;
  rndis_config.requested_mtu = config_.mtu;
  state_machine_ = std::make_unique<rndis::StateMachine>(*control_channel_, *this, rndis_config);

  TETHERKITNEXT_RETURN_IF_ERROR(state_machine_->Start());

  // Only here do we have the negotiation result: device MAC, final MTU, aggregation limit, alignment requirement.
  const rndis::NegotiatedParameters& parameters = state_machine_->Parameters();
  const rndis::DeviceInfo& info = state_machine_->Info();

  // ---- Step 4: create the feth pair ----
  //
  // It must come after negotiation: the system-side MAC must be set to the address the device reported, the MTU must use the negotiation result,
  // and the MAC must be set before the interface goes IFF_UP.
  {
    const net::MacAddress* system_mac = nullptr;
    net::MacAddress adopted{};
    if (config_.adopt_device_mac && info.has_permanent_address) {
      std::ranges::copy(info.permanent_address, adopted.begin());
      system_mac = &adopted;
    }
    TETHERKITNEXT_ASSIGN_OR_RETURN(auto pair, net::FethPair::Create(parameters.mtu, system_mac));
    feth_pair_ = std::make_unique<net::FethPair>(std::move(pair));
  }
  // Flush into the snapshot as soon as the NIC is built: if any later step fails, the GUI should also be able to see the NIC name,
  // otherwise the user would not even know "which NIC was just built".
  RefreshSnapshot();

  // ---- Step 5: open BPF on the **driver-side** interface ----
  //
  // Attaching to the driver side rather than the system side is because frames the host sends out of the system side appear as the input direction on the driver side,
  // while frames we write into the driver side enter the system side's input -- one descriptor handles both receiving and sending.
  {
    net::BpfConfig bpf_config = config_.bpf;
    // Per-frame limit = MTU + Ethernet header. Note that the kernel bpfwrite's hard cap is MTU + 18,
    // so it must never be exceeded here.
    bpf_config.max_frame_bytes = parameters.mtu + rndis::kEthernetHeaderBytes;
    TETHERKITNEXT_ASSIGN_OR_RETURN(bpf_link_,
                               net::BpfLink::Open(feth_pair_->DriverSide().Name(), bpf_config));
  }

  // ---- Step 6: create the USB data channel ----
  {
    usb::DataChannelConfig data_config = config_.data_channel;
    // The bulk IN buffer must be >= the MaxTransferSize we claim in INITIALIZE_MSG,
    // otherwise the large transfers the device aggregates would overflow / be truncated.
    data_config.rx_transfer_bytes =
        std::max(data_config.rx_transfer_bytes, config_.rndis.host_max_transfer_size);
    TETHERKITNEXT_ASSIGN_OR_RETURN(data_channel_,
                               usb::UsbDataChannel::Create(*device_, parameters, data_config));
  }

  // ---- Step 7: start the bridge layer ----
  {
    BridgeConfig bridge_config = config_.bridge;
    bridge_config.max_frame_bytes = parameters.mtu + rndis::kEthernetHeaderBytes;
    bridge_ = std::make_unique<Bridge>(*data_channel_, *bpf_link_, bridge_config);
    TETHERKITNEXT_RETURN_IF_ERROR(bridge_->Start());
  }

  RefreshSnapshot();
  PrintNextSteps();
  return Ok();
}

void Runtime::RunControlLoop() {
  BridgeStats previous = bridge_->Snapshot();
  Nanos last_stats_nanos = MonotonicNanos();

  const Nanos stats_interval_nanos =
      static_cast<Nanos>(config_.stats_interval_millis) * kNanosPerMilli;

  while (!stop_requested_.load(std::memory_order_acquire) &&
         !fatal_error_.load(std::memory_order_acquire)) {
    // ---- Periodic work of the state machine: keepalive + draining messages pushed by the device ----
    if (const auto status = state_machine_->Poll(); !status) {
      RecordFatal(std::move(status).error());
      break;
    }

    // ---- Refresh the snapshot ----
    //
    // Done in every loop iteration rather than only when events occur: the statistics counters change continuously,
    // and the GUI computes rates from the difference of two snapshots. The loop period cap is 250 ms, enough for a 2 Hz UI refresh.
    RefreshSnapshot();

    // ---- Periodic statistics ----
    const Nanos now = MonotonicNanos();
    if (stats_interval_nanos != 0 && now - last_stats_nanos >= stats_interval_nanos) {
      const BridgeStats current = bridge_->Snapshot();
      const double seconds =
          static_cast<double>(now - last_stats_nanos) / static_cast<double>(kNanosPerSecond);
      TETHERKITNEXT_INFO("{}", FormatStatsLine(previous, current, seconds));
      previous = current;
      last_stats_nanos = now;
    }

    // ---- Sleep until the next moment we need to wake ----
    //
    // Take the nearer of the two deadlines "next keepalive" and "next statistics", to avoid needless wakeups.
    // The 250 ms cap is so that RequestStop(), which only writes an atomic, is also noticed in time.
    std::uint32_t sleep_millis =
        std::min<std::uint32_t>(state_machine_->MillisUntilNextPoll(), 250);
    if (stats_interval_nanos != 0) {
      const Nanos elapsed = now - last_stats_nanos;
      const Nanos remaining = elapsed >= stats_interval_nanos ? 0 : stats_interval_nanos - elapsed;
      sleep_millis = std::min<std::uint32_t>(sleep_millis,
                                             static_cast<std::uint32_t>(remaining / kNanosPerMilli));
    }
    // Sleep at least 10 ms: to avoid spinning busily right at the boundary when the keepalive has just expired.
    sleep_millis = std::max<std::uint32_t>(sleep_millis, 10);

    // A condition variable is used rather than sleep_for: Stop() can wake us immediately, so when the user presses "Stop"
    // there is no need to wait up to 250 ms for a reaction.
    std::unique_lock<std::mutex> lock(stop_mutex_);
    stop_condition_.wait_for(lock, std::chrono::milliseconds(sleep_millis),
                             [this] { return stop_requested_.load(std::memory_order_acquire); });
  }

  if (fatal_error_.load(std::memory_order_acquire)) {
    TETHERKITNEXT_ERROR_TR(Msg::kCoreExitingOnFatal);
  }
}

void Runtime::PrintNextSteps() const {
  const std::string_view name = feth_pair_->SystemSide().Name();
  TETHERKITNEXT_INFO("");
  TETHERKITNEXT_INFO_TR(Msg::kCoreInterfaceReady, name);
  TETHERKITNEXT_INFO_TR(Msg::kCoreNextStepsAssignIp);
  TETHERKITNEXT_INFO("    sudo ipconfig set {} DHCP", name);
  TETHERKITNEXT_INFO_TR(Msg::kCoreNextStepsVerify);
  TETHERKITNEXT_INFO("    ipconfig getifaddr {}", name);
  TETHERKITNEXT_INFO("    ipconfig getsummary {}", name);
  TETHERKITNEXT_INFO_TR(Msg::kCoreNextStepsDefaultRoute);
  TETHERKITNEXT_INFO("    sudo route -n change default $(ipconfig getoption {} router)", name);
  TETHERKITNEXT_INFO_TR(Msg::kCoreNextStepsTemporaryNote1);
  TETHERKITNEXT_INFO_TR(Msg::kCoreNextStepsTemporaryNote2);
  TETHERKITNEXT_INFO("");
}

// =============================================================================
// Shutdown
// =============================================================================

void Runtime::Teardown() {
  TETHERKITNEXT_INFO_TR(Msg::kCoreStopping);

  // Tear down in the strict **reverse** of the startup order.
  //
  // 1. Stop the bridge layer first: internally it joins the two data-path threads, then calls
  //    DataChannel::Shutdown() to wait for all in-flight USB transfers to be reclaimed. This step must complete before destroying
  //    device_ -- libusb_close will not reclaim in-flight transfers for us,
  //    and closing the handle first then freeing the transfers is a use-after-free.
  if (bridge_ != nullptr) {
    bridge_->Stop();
    bridge_.reset();
  }
  data_channel_.reset();

  // 2. Close BPF (nobody is reading or writing it any more by now).
  bpf_link_.reset();

  // 3. Destroy the feth pair. The kernel's feth_clone_destroy automatically unpairs the peer first.
  feth_pair_.reset();

  // 4. Make the device leave RNDIS: first SET filter = 0 to make it stop sending data, then send HALT.
  //    This must be done before closing the USB handle -- otherwise the device will keep thinking the host is still there,
  //    and the state will not be clean the next time it is plugged in.
  //
  //    This step sends synchronous control messages, so it must be on the control thread -- and this function is.
  if (state_machine_ != nullptr) {
    state_machine_->Stop();
    state_machine_.reset();
  }
  // First stop interrupt listening (it waits for the in-flight asynchronous transfers to be reclaimed), then destroy the channel object.
  if (control_channel_ != nullptr) {
    control_channel_->StopNotificationListener();
    control_channel_.reset();
  }

  // 5. Release the interfaces and close the device handle.
  device_.reset();

  // 6. Finally stop the libusb event thread and release the context.
  usb_context_.reset();

  // Refresh once more after teardown: the NIC name and statistics should be zeroed, and the GUI should not keep showing a NIC that no longer exists.
  // link_up is maintained by events and RefreshSnapshot does not touch it, so it is cleared explicitly here --
  // a UI still lit up with "link connected" after shutdown would be misleading.
  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    snapshot_.link_up = false;
  }
  RefreshSnapshot();
  TETHERKITNEXT_INFO_TR(Msg::kCoreStopped);
}

// =============================================================================
// Snapshots and events
// =============================================================================

void Runtime::RefreshSnapshot() {
  // First read the state of each component outside the lock -- these reads go through ioctl / atomic loads,
  // and should not occupy snapshot_mutex_.
  RuntimeSnapshot fresh;
  if (device_ != nullptr) {
    fresh.device_description = std::string{device_->Describe()};
  }
  if (state_machine_ != nullptr) {
    fresh.rndis_state = state_machine_->CurrentState();
    fresh.device_info = state_machine_->Info();
    fresh.parameters = state_machine_->Parameters();
  }
  if (feth_pair_ != nullptr) {
    fresh.system_interface = std::string{feth_pair_->SystemSide().Name()};
    fresh.driver_interface = std::string{feth_pair_->DriverSide().Name()};
  }
  if (bridge_ != nullptr) {
    fresh.bridge = bridge_->Snapshot();
    fresh.paused = bridge_->Paused();
  }

  const std::lock_guard<std::mutex> guard(snapshot_mutex_);
  // run_state, link_up and fatal_message are maintained at their own transition points; they must not be overwritten here.
  fresh.run_state = snapshot_.run_state;
  fresh.link_up = snapshot_.link_up;
  fresh.fatal_message = snapshot_.fatal_message;
  snapshot_ = std::move(fresh);
}

void Runtime::SetRunState(RunState next) {
  RunState previous = RunState::kIdle;
  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    previous = snapshot_.run_state;
    if (previous == next) {
      return;
    }
    snapshot_.run_state = next;
  }
  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kRunState,
                    .a = static_cast<std::int64_t>(previous),
                    .b = static_cast<std::int64_t>(next),
                    .text = std::string{RunStateName(next)}});
}

void Runtime::RecordFatal(const Error& error) {
  const std::string message = error.ToString();
  TETHERKITNEXT_ERROR("{}", message);

  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    // Record only the **first** fatal error: later ones are mostly chain reactions caused by it, and overwriting would lose the root cause.
    if (snapshot_.fatal_message.empty()) {
      snapshot_.fatal_message = message;
    }
  }
  fatal_error_.store(true, std::memory_order_release);
  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kFatal, .text = message});
}

void Runtime::Emit(const RuntimeEvent& event) const {
  if (config_.event_sink != nullptr) {
    config_.event_sink->OnRuntimeEvent(event);
  }
}

// =============================================================================
// StateMachineObserver (all invoked on the control thread)
// =============================================================================

void Runtime::OnStateChanged(rndis::State from, rndis::State to) {
  // The state machine has already logged, so it is not repeated here; only the transition is forwarded to the host.
  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kRndisState,
                    .a = static_cast<std::int64_t>(from),
                    .b = static_cast<std::int64_t>(to),
                    .text = std::string{rndis::StateName(to)}});
}

void Runtime::OnNegotiated(const rndis::NegotiatedParameters& parameters,
                           const rndis::DeviceInfo& info) {
  TETHERKITNEXT_INFO_TR(Msg::kCoreRndisReady, rndis::FormatMac(info.permanent_address).data(),
                    parameters.mtu, info.LinkSpeedMbps());

  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kNegotiated,
                    .a = static_cast<std::int64_t>(parameters.mtu),
                    .b = static_cast<std::int64_t>(info.LinkSpeedMbps()),
                    .text = info.vendor_description});
}

void Runtime::OnLinkStateChanged(bool connected) {
  TETHERKITNEXT_INFO_TR(Msg::kCoreLinkState,
                    Text(connected ? Msg::kCoreLinkConnected : Msg::kCoreLinkDisconnected));

  {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    snapshot_.link_up = connected;
  }

  // Pause data movement when the link goes down. Continuing to send frames onto a disconnected link is only a waste, and would fill up the queue,
  // so that upon real recovery a pile of stale frames would be sent out first.
  if (bridge_ != nullptr) {
    bridge_->SetPaused(!connected);
  }

  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kLink, .a = connected ? 1 : 0});
}

void Runtime::OnDeviceReset(bool addressing_lost) {
  TETHERKITNEXT_WARN_TR(Msg::kCoreDeviceReset,
                    Text(addressing_lost ? Msg::kCoreAddressingLost : Msg::kCoreAddressingKept));

  // During a reset the device discarded all outstanding data packets. A brief pause lets the state machine finish replaying the packet filter,
  // avoiding continued flooding of data during the window when the device rebuilds its internal state.
  if (bridge_ != nullptr) {
    bridge_->SetPaused(true);
    bridge_->SetPaused(false);
  }

  Emit(RuntimeEvent{.kind = RuntimeEvent::Kind::kDeviceReset, .a = addressing_lost ? 1 : 0});
}

void Runtime::OnFatalError(const Error& error) {
  // WithContext is &&-qualified, while what we hold here is a const reference, so a copy must be made first.
  Error annotated = error;
  RecordFatal(std::move(annotated).WithContext(Tr(Msg::kCoreLinkUnrecoverable)));
}

}  // namespace tetherkitnext::core
