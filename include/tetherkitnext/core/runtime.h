// Runtime orchestration: assembles USB, the RNDIS state machine, feth, BPF and the bridge into a runnable driver.
//
// * Startup order (each step depends on the product of the previous one; they cannot be swapped) *
//
//   1. Permission check     -- give a plain-language error early, rather than letting a pile of EPERM pop up later
//   2. libusb context + event thread
//   3. Discover and open the RNDIS device (claim the two interfaces, parse the endpoints)
//   4. RNDIS control channel + state machine Start()
//        └→ Only this step yields: the device MAC, the negotiated MTU, the aggregation limit, the alignment requirement
//   5. Use the result of step 4 to create the feth pair (system-side MAC = the MAC the device reported)
//   6. Open BPF on the driver-side interface
//   7. Use the negotiated parameters of step 4 to create the USB data channel
//   8. Start the bridge layer
//   9. The control thread enters its loop: periodic Poll() (keepalive + draining device pushes)
//
//   Why feth must be created **after** RNDIS negotiation: the MAC of the system-side NIC must be set to
//   the OID_802_3_PERMANENT_ADDRESS reported by the device, and the MTU must use the negotiation result -- neither value can be obtained
//   before step 4. And the MAC must be set before the interface goes IFF_UP.
//
// * Shutdown order is the strict reverse of startup order *
//
//   Bridge layer -> data channel (wait for in-flight transfers to be reclaimed) -> BPF -> feth -> state machine HALT -> device -> libusb
//
//   The step "data channel waits for in-flight transfers to be reclaimed" must complete **before** destroying the device,
//   otherwise it is a use-after-free (see the explanation in usb/data_channel.h).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/core/bridge.h"
#include "tetherkitnext/net/bpf_link.h"
#include "tetherkitnext/net/feth_device.h"
#include "tetherkitnext/rndis/state_machine.h"
#include "tetherkitnext/usb/context.h"
#include "tetherkitnext/usb/data_channel.h"
#include "tetherkitnext/usb/device.h"

namespace tetherkitnext::core {

/// Lifecycle state of the runtime. This is the only state dimension the host (CLI / GUI) needs to care about.
enum class RunState : std::uint8_t {
  kIdle,      ///< Created, Start() not yet called.
  kStarting,  ///< The control thread is running the startup sequence (enumerate -> handshake -> create NIC -> open bridge).
  kRunning,   ///< The data path is up and running.
  kStopping,  ///< Being torn down in reverse order.
  kStopped,   ///< Stopped normally.
  kFailed,    ///< Startup failed or an unrecoverable error occurred while running; see RuntimeSnapshot::fatal_message for the cause.
};

[[nodiscard]] std::string_view RunStateName(RunState state) noexcept;

/// An event the runtime reports outward.
///
/// "Events + the host queues them itself" is used instead of having the host implement StateMachineObserver directly:
/// the latter's five callbacks have varied semantics and take C++ references, which cross-language hosts cannot use; and in a callback the host
/// can easily call back into the runtime by accident (e.g. "stop on receiving a fatal error"), which is a self-wait deadlock.
struct RuntimeEvent {
  enum class Kind : std::uint8_t {
    kRndisState,   ///< a = rndis::State before the transition, b = after.
    kNegotiated,   ///< a = final MTU, b = link speed (Mbps).
    kLink,         ///< a = 1 means the link is connected.
    kDeviceReset,  ///< a = 1 means addressing information was lost and has been replayed.
    kFatal,        ///< text = the cause of the unrecoverable error.
    kRunState,     ///< a = RunState before the transition, b = after.
  };

  Kind kind = Kind::kRunState;
  std::int64_t a = 0;
  std::int64_t b = 0;
  std::string text;
};

/// Event sink: the host implements it to receive runtime events.
///
/// * Two hard constraints *
///   1. Callbacks **always occur on the control thread** (all event sources are on that one thread), but the host's own
///      thread may be calling Snapshot() at the same time -- not a single lock that should be added in the implementation may be omitted.
///   2. **The implementation must never call any Runtime method.** Stop() has to join the control thread,
///      and calling it from an event is a self-wait deadlock. The correct approach is to queue the event and let the host thread handle it.
class RuntimeEventSink {
 public:
  RuntimeEventSink() = default;
  RuntimeEventSink(const RuntimeEventSink&) = delete;
  RuntimeEventSink& operator=(const RuntimeEventSink&) = delete;
  RuntimeEventSink(RuntimeEventSink&&) = delete;
  RuntimeEventSink& operator=(RuntimeEventSink&&) = delete;
  virtual ~RuntimeEventSink() = default;

  virtual void OnRuntimeEvent(const RuntimeEvent& event) = 0;
};

/// A complete observable snapshot of the runtime.
///
/// Why copy the whole block rather than provide a bunch of per-item accessors: what the host needs is a **consistent** set of values
/// (a UI whose state does not match the NIC name is hard to make sense of), and per-item reads would give torn combinations. One copy is
/// a few hundred bytes, which is no problem at all for a 2 Hz refresh rate.
struct RuntimeSnapshot {
  RunState run_state = RunState::kIdle;
  rndis::State rndis_state = rndis::State::kUninitialized;
  bool link_up = false;
  /// Whether data movement is paused (during link down or a device soft reset).
  bool paused = false;

  /// System-side NIC name (the host configures IP on this one). Empty when not created.
  std::string system_interface;
  /// Driver-side NIC name (BPF attaches to this one). For troubleshooting display only.
  std::string driver_interface;
  /// In the form "Bus 020 Device 003: 18d1:4ee4". Empty when no device is open.
  std::string device_description;

  rndis::DeviceInfo device_info;
  rndis::NegotiatedParameters parameters;
  BridgeStats bridge;

  /// The cause when run_state == kFailed; otherwise empty.
  std::string fatal_message;
};

/// Runtime configuration. The derivation of the default values is explained in each field's comment.
struct RuntimeConfig {
  /// Device filter. All 0 means use the first RNDIS device found.
  usb::DeviceFilter device_filter;

  /// The desired MTU. Lowered by negotiation when the device cannot fit it.
  ///
  /// The upper limit is constrained by feth's sysctl net.link.fake.max_mtu (2048 on this machine).
  std::uint32_t mtu = rndis::kDefaultMtu;

  /// Specifies the feth interface name (such as "feth7"); empty means let the kernel choose the number automatically.
  std::string system_interface_name;
  std::string driver_interface_name;

  /// Whether to set the system-side NIC's MAC to the address the device reported.
  ///
  /// On by default: under RNDIS semantics the device is this NIC, and the peer's ARP table and DHCP lease are both built on this
  /// MAC. Turning it off is only useful when investigating MAC conflicts.
  bool adopt_device_mac = true;

  rndis::StateMachineConfig rndis;
  usb::DataChannelConfig data_channel;
  net::BpfConfig bpf;
  BridgeConfig bridge;

  /// Statistics report period (milliseconds); 0 means no reports.
  std::uint32_t stats_interval_millis = 5'000;

  /// Event sink. nullptr means no event reports are needed (the CLI does not need them; it reads the logs).
  ///
  /// The lifetime is the caller's responsibility; it must outlive the Runtime.
  RuntimeEventSink* event_sink = nullptr;
};

/// Assembles and runs the whole driver.
///
/// * Threading model: Start() is **non-blocking** *
///
///   Runtime owns a control thread itself, and the startup sequence, the keepalive loop and the shutdown teardown all
///   run on it **entirely**. The host thread is only responsible for issuing commands and reading snapshots.
///
///   Why it must be this way:
///     * The startup sequence includes the USB handshake, which takes hundreds of milliseconds to several seconds on slow devices; blocking the GUI main thread is not acceptable;
///     * The control channel uses libusb's synchronous API, and the synchronous API returns
///       LIBUSB_ERROR_BUSY on the libusb event thread -- having Runtime create its own thread removes the need to require that the host "must
///       call from some specific thread", a convention that is very easy to violate;
///     * The teardown order includes RNDIS's graceful shutdown (control messages must be sent); putting it on the same
///       thread as well naturally satisfies `StateMachine`'s constraint that "all methods are called on the same thread".
///
/// Usage:
/// ```
/// TETHERKITNEXT_ASSIGN_OR_RETURN(auto runtime, Runtime::Create(config));
/// TETHERKITNEXT_RETURN_IF_ERROR(runtime->Start());   // returns immediately
/// runtime->WaitUntilStopped();                   // CLI: park the main thread
/// runtime->Stop();                               // idempotent
/// ```
/// The GUI does not call WaitUntilStopped; instead it calls Snapshot() periodically to refresh the UI.
class Runtime final : public rndis::StateMachineObserver {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Runtime>> Create(const RuntimeConfig& config);

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  ~Runtime() override;

  /// Starts the control thread and returns immediately.
  ///
  /// A successful return only means "the start request has been accepted"; real success or failure must be checked via Snapshot().run_state.
  /// The only thing that returns failure **synchronously** is the root check -- it needs no I/O, and reporting it early lets the
  /// command line give an immediate plain-language hint.
  [[nodiscard]] Status Start();

  /// Blocks until the runtime stops (normal shutdown or fatal error). The command line uses it to park the main thread.
  void WaitUntilStopped();

  /// Requests shutdown. May be called from a signal handler or any thread; async-signal-safe (only writes an atomic).
  ///
  /// The cost is that the control thread takes up to one loop period (<=250 ms) to see it. To stop immediately use
  /// Stop(), which additionally wakes the control thread -- but Stop() takes a lock and is **not** async-signal-safe.
  void RequestStop() noexcept { stop_requested_.store(true, std::memory_order_release); }

  /// Requests shutdown and waits for the control thread to finish all teardown. Idempotent.
  ///
  /// WARNING: Must not be called from the event sink (RuntimeEventSink) -- that is the control thread itself, a self-wait deadlock.
  void Stop();

  /// Takes a consistent state snapshot. **Callable from any thread**.
  [[nodiscard]] RuntimeSnapshot Snapshot() const;

  /// System-side NIC name, used to print "what to do next" for the user.
  [[nodiscard]] std::string SystemInterfaceName() const;

  // ---- StateMachineObserver (all invoked on the control thread) ----
  void OnStateChanged(rndis::State from, rndis::State to) override;
  void OnNegotiated(const rndis::NegotiatedParameters& parameters,
                    const rndis::DeviceInfo& info) override;
  void OnLinkStateChanged(bool connected) override;
  void OnDeviceReset(bool addressing_lost) override;
  void OnFatalError(const Error& error) override;

 private:
  Runtime() = default;

  /// Control thread body: startup sequence -> control loop -> teardown.
  void RunControlThread() noexcept;

  /// The complete startup sequence (the body of the original Start()). Runs only on the control thread.
  [[nodiscard]] Status RunStartSequence();

  /// The loop of keepalive + statistics + shutdown checks. Runs only on the control thread.
  void RunControlLoop();

  /// Tears down in the strict reverse of the startup order. Runs only on the control thread.
  void Teardown();

  /// Transitions the lifecycle state and reports the event.
  void SetRunState(RunState next);

  /// Records a fatal error: writes it into the snapshot, sets the kFailed flag, reports the event.
  void RecordFatal(const Error& error);

  /// Refreshes the current state of each component into the snapshot. Called only on the control thread.
  void RefreshSnapshot();

  /// Hands an event to the host. A no-op when the sink is null.
  void Emit(const RuntimeEvent& event) const;

  /// Prints the hint "the NIC is ready; here is how to configure the IP next".
  void PrintNextSteps() const;

  RuntimeConfig config_;

  // ---- The following components are **created, accessed and destroyed only by the control thread** ----
  //
  // The host thread always reads state through snapshot_ and never touches these pointers -- otherwise the reset() in Stop()
  // and the host's reads would constitute a data race, which on real hardware shows up as random crashes.
  std::unique_ptr<usb::Context> usb_context_;
  std::unique_ptr<usb::Device> device_;
  std::unique_ptr<usb::UsbControlChannel> control_channel_;
  std::unique_ptr<rndis::StateMachine> state_machine_;
  std::unique_ptr<net::FethPair> feth_pair_;
  std::unique_ptr<net::BpfLink> bpf_link_;
  std::unique_ptr<usb::UsbDataChannel> data_channel_;
  std::unique_ptr<Bridge> bridge_;

  std::thread control_thread_;

  /// Protects snapshot_. The reading side is the host thread (the GUI, once every 500 ms), the writing side is the control thread,
  /// contention is extremely light, and an ordinary mutex suffices.
  mutable std::mutex snapshot_mutex_;
  RuntimeSnapshot snapshot_;

  /// Lets Stop() immediately wake a sleeping control thread, without waiting for a full loop period.
  std::mutex stop_mutex_;
  std::condition_variable stop_condition_;

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> fatal_error_{false};

  /// Read and written only on the host thread (Start / Stop / destructor); no synchronization needed.
  bool started_ = false;
  bool stopped_ = false;
};

}  // namespace tetherkitnext::core
