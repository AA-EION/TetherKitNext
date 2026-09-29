// RNDIS host-side state machine.
//
// * State diagram (the spec defines three device-side states; the host side mirrors them and adds transition states) *
//
//                      ┌──────────────────┐
//        ┌────────────►│  kUninitialized  │◄────────────┐
//        │             └────────┬─────────┘             │
//        │                      │ Start()               │ HALT reply /
//        │                      │ send INITIALIZE_MSG   │ bus gone / fatal error
//        │                      ▼                       │
//        │             ┌──────────────────┐             │
//        │             │  kInitializing   ├─────────────┤
//        │             └────────┬─────────┘  nego. fail │
//        │                      │ recv INITIALIZE_CMPLT │
//        │                      │ and negotiation OK    │
//        │                      ▼                       │
//        │             ┌──────────────────┐             │
//        │             │  kInitialized    │             │  <- control messages usable,
//        │             └────────┬─────────┘             │    but data channel **not open**
//        │                      │ QUERY MAC/link etc.   │
//        │                      │ SET packet filter≠0   │
//        │                      ▼                       │
//        │             ┌──────────────────┐             │
//        │        ┌────┤ kDataInitialized ├─────────────┤
//        │        │    └────────┬─────────┘             │
//        │        │             │ Stop()                │
//        │  SET filter=0       │ send HALT_MSG         │
//        │  (back one state)    ▼                       │
//        │             ┌──────────────────┐             │
//        └─────────────┤    kHalting      ├─────────────┘
//                      └──────────────────┘
//
//   Semantics of the spec's original text:
//     * After bus initialization the device is in RNDIS-uninitialized;
//     * On receiving INITIALIZE_MSG and replying INITIALIZE_CMPLT(SUCCESS) -> RNDIS-initialized;
//     * On receiving SET(OID_GEN_CURRENT_PACKET_FILTER, **non-zero**) -> RNDIS-data-initialized,
//       **only then does data start flowing**;
//     * In data-initialized, on receiving SET(filter = 0) -> back to RNDIS-initialized;
//     * At any time, on receiving HALT_MSG or a bus disconnect -> immediately back to RNDIS-uninitialized.
//
// * Three easy-to-get-wrong protocol details *
//
//   1. **Control requests must be serialized.** GET_ENCAPSULATED_RESPONSE can only "take the next queued
//      response"; it has no ability to select by RequestId. So only one request can be in flight at a time.
//
//   2. **The device cuts in.** While waiting for some *_CMPLT, the device may well first slip in a
//      REMOTE_NDIS_INDICATE_STATUS_MSG (media connect status change) or a **device-initiated**
//      REMOTE_NDIS_KEEPALIVE_MSG (in which case the host must reply KEEPALIVE_CMPLT, otherwise the device may
//      consider the host dead and disconnect). So the control read loop must be written as a dispatcher: dispatch by MessageType,
//      rather than "blindly assuming that what was read is the one I want".
//
//   3. **RESET has no RequestId.** RESET_MSG's offset 8 is Reserved,
//      and RESET_CMPLT's offset 8 is Status (not 12). So RESET cannot be paired by ID,
//      and only one can be in flight at a time. Moreover, when AddressingReset in RESET_CMPLT is non-zero,
//      the host **must resend** SET(OID_GEN_CURRENT_PACKET_FILTER) (and if a multicast list was set, also resend
//      SET(OID_802_3_MULTICAST_LIST)) -- otherwise data will not flow again after the reset.
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

/// Host-side state.
enum class State : std::uint8_t {
  kUninitialized,     ///< Uninitialized. The control channel is usable, but the device has not yet agreed to start work.
  kInitializing,      ///< INITIALIZE_MSG sent, waiting for INITIALIZE_CMPLT.
  kInitialized,       ///< Negotiation complete. OIDs can be read and written, but the **data channel is not open yet**.
  kDataInitialized,   ///< A non-zero packet filter has been set; data starts flowing.
  kHalting,           ///< HALT_MSG sent, wrapping up.
};

[[nodiscard]] std::string_view StateName(State state) noexcept;

/// Device information reported by the device that the host cares about.
struct DeviceInfo {
  /// The device's permanent MAC. This is the address the host-side feth should adopt -- under RNDIS semantics the device is
  /// that NIC, and the peer's ARP table and DHCP lease are both built on it.
  MacAddress permanent_address{};
  /// Current MAC. Same as permanent for most devices.
  MacAddress current_address{};
  bool has_permanent_address = false;
  bool has_current_address = false;

  /// Link speed, **in units of 100 bps** (this is NDIS's convention, not bps). 0 means disconnected.
  std::uint32_t link_speed_100bps = 0;
  /// Maximum frame length, **excluding** the 14-byte Ethernet header. Typically 1500.
  std::uint32_t maximum_frame_size = 0;
  /// Physical medium. An optional OID; stays kUnspecified if it cannot be queried.
  PhysicalMedium physical_medium = PhysicalMedium::kUnspecified;
  /// Media connect status. **Note that 0 means connected.**
  MediaState media_state = MediaState::kConnected;
  /// Vendor description string (NUL termination not guaranteed; already normalized into std::string here).
  std::string vendor_description;
  std::uint32_t vendor_id = 0;

  /// Link speed converted to Mbps, used only for logging.
  [[nodiscard]] double LinkSpeedMbps() const noexcept {
    return static_cast<double>(link_speed_100bps) * 100.0 / 1e6;
  }
};

/// Configuration of the state machine.
struct StateMachineConfig {
  /// The desired MTU. Lowered by Negotiate when the device cannot fit it.
  std::uint32_t requested_mtu = kDefaultMtu;

  /// The MaxTransferSize the host claims in INITIALIZE_MSG, which is also the size of our bulk IN buffer.
  ///
  /// **This is the most important lever for RNDIS throughput**: it is the **only** means of getting the device to aggregate multiple REMOTE_NDIS_PACKET_MSGs
  /// into one bulk IN. The larger the reported value, the more the device aggregates, and the lower the USB
  /// and system-call overhead amortized per frame. Linux conservatively reports only 2048 (one full frame); we report 16 KiB --
  /// leaving ample room for aggregation while not exceeding the spec's 0x4000 cap for USB 1.1 devices.
  std::uint32_t host_max_transfer_size = 16 * 1024;

  /// How many PACKET_MSGs to aggregate at most per bulk OUT in the host -> device direction. 0 means no extra limit,
  /// fully following the MaxPacketsPerMessage reported by the device.
  ///
  /// This knob exists because the aggregation capability the device reports is **not necessarily trustworthy**. Mainline Linux gadget's
  /// `rndis_rm_hdr()` splits only one PACKET_MSG per bulk OUT, while a vendor kernel may still report
  /// MaxPacketsPerMessage=10. If such a device is really encountered, all frames except the first would be silently dropped by the peer,
  /// and clamping to 1 falls back to "one frame per transfer".
  ///
  /// WARNING: **But do not take it as the default explanation for dropped frames.** On the Android device that was measured
  /// (reporting 10 packets / 15800 bytes) an A/B test was done: clamping to 1 was actually slower (TCP TX 125 vs 234 Mbps),
  /// and the drop rate did not improve either -- that device's aggregation **really works**. So this is a diagnostic / fallback
  /// knob, not a switch for a known defect. See "real-device end-to-end measurements" in docs/BENCHMARKS.md.
  std::uint32_t max_tx_packets_per_message = 0;

  /// The packet filter bit mask to set.
  std::uint32_t packet_filter = kDefaultPacketFilter;

  /// Control transfer timeout (milliseconds).
  std::uint32_t control_timeout_millis = kControlTimeoutMillis;

  /// Keepalive period (milliseconds).
  ///
  /// The semantics are to send only when "this long has passed since any message was last received from the device", not to send unconditionally on a timer.
  /// Note that libusb control transfers on Apple Silicon are about 10x slower than on x64 (issue #1288),
  /// so do not set this value too small.
  std::uint32_t keepalive_interval_millis = kKeepAliveTimeoutMillis;

  /// Upper limit on the number of response polls for a single control request.
  ///
  /// Linux's approach is to poll up to 10 times, 40 ms apart (about 400 ms in total). We wait for the interrupt
  /// endpoint's notification first and poll after a timeout, so this is mainly a backstop for devices "without an interrupt endpoint".
  std::uint32_t response_poll_attempts = 10;

  /// Wait between polls (milliseconds).
  std::uint32_t response_poll_interval_millis = 40;

  /// After how many consecutive keepalive failures the link is judged dead.
  std::uint32_t keepalive_failure_threshold = 3;
};

/// Events the state machine reports outward.
///
/// Callbacks are used instead of having the caller poll the state, because events such as "media connect status change" are actively
/// pushed by the device (INDICATE_STATUS), and polling would miss the edges.
class StateMachineObserver {
 public:
  StateMachineObserver() = default;
  StateMachineObserver(const StateMachineObserver&) = delete;
  StateMachineObserver& operator=(const StateMachineObserver&) = delete;
  StateMachineObserver(StateMachineObserver&&) = delete;
  StateMachineObserver& operator=(StateMachineObserver&&) = delete;
  virtual ~StateMachineObserver() = default;

  /// State transition.
  virtual void OnStateChanged(State from, State to) = 0;

  /// Negotiation complete; the data path can be built with these parameters.
  virtual void OnNegotiated(const NegotiatedParameters& parameters, const DeviceInfo& info) = 0;

  /// Link up / down (from MEDIA_CONNECT / MEDIA_DISCONNECT in INDICATE_STATUS,
  /// or the result of QUERY OID_GEN_MEDIA_CONNECT_STATUS).
  virtual void OnLinkStateChanged(bool connected) = 0;

  /// Device reset complete, and the host is required to resend addressing configuration. The data path needs to pause briefly.
  virtual void OnDeviceReset(bool addressing_lost) = 0;

  /// The link is no longer usable (consecutive keepalive failures / device disconnected / unrecoverable protocol error).
  virtual void OnFatalError(const Error& error) = 0;
};

/// The RNDIS host-side state machine.
///
/// **Thread constraint: all methods of this class must be called from the same thread** (the control thread). This is not the implementation
/// being lazy, but a hard libusb requirement -- synchronous control transfers return
/// LIBUSB_ERROR_BUSY on the event thread, so the control channel must exclusively own a non-event thread.
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

  /// Runs the complete startup sequence, up to kDataInitialized.
  ///
  /// Sequence (the order follows Linux's generic_rndis_bind, and every step has a basis):
  ///   1. INITIALIZE_MSG -> INITIALIZE_CMPLT, negotiating MaxTransferSize / alignment / MTU;
  ///   2. QUERY OID_GEN_PHYSICAL_MEDIUM (**optional OID, failure is not fatal**);
  ///   3. QUERY OID_802_3_PERMANENT_ADDRESS (get the MAC the host side will use);
  ///   4. QUERY OID_802_3_CURRENT_ADDRESS / OID_GEN_MAXIMUM_FRAME_SIZE /
  ///      OID_GEN_LINK_SPEED / OID_GEN_MEDIA_CONNECT_STATUS (all non-fatal);
  ///   5. SET OID_GEN_CURRENT_PACKET_FILTER = non-zero -> data starts flowing.
  /// Failure of any fatal step sends HALT_MSG and then returns the error.
  [[nodiscard]] Status Start();

  /// Graceful shutdown: SET filter = 0, then send HALT_MSG.
  ///
  /// Even if an error occurs midway, it makes a best effort to send the HALT -- if it is not sent, the device may keep thinking the host
  /// is still there, and the state will not be clean the next time it is plugged in.
  void Stop();

  /// Does one round of "periodic work that should be done". Should be called periodically in a loop by the control thread.
  ///
  /// Does two things:
  ///   * When more than the keepalive period has passed since a message was last received from the device, sends one KEEPALIVE;
  ///   * Drains messages actively pushed by the device on the control channel (INDICATE_STATUS / device-initiated
  ///     KEEPALIVE), not letting them pile up.
  ///
  /// Returning an error means the link is no longer usable, and the caller should reconnect.
  [[nodiscard]] Status Poll();

  /// How many milliseconds remain until the next periodic work is due. Lets the caller decide how long to sleep.
  [[nodiscard]] std::uint32_t MillisUntilNextPoll() const noexcept;

  /// Actively requests a soft reset of the device.
  ///
  /// RESET is a soft reset: the control channel stays intact, but the device discards all outstanding requests and data packets.
  /// If AddressingReset in RESET_CMPLT is non-zero, this method automatically resends the packet filter setting.
  [[nodiscard]] Status Reset();

 private:
  /// Result of one control request-response round trip.
  struct Exchange {
    /// Byte view of the response message, pointing into the control channel's internal buffer.
    std::span<const std::byte> response;
  };

  /// Sends a request, then waits for its response.
  ///
  /// In the meantime the device's cut-in messages (INDICATE_STATUS / device-initiated KEEPALIVE) are handled correctly:
  /// they are dispatched away and it keeps waiting for the one we want.
  [[nodiscard]] Result<Exchange> Transact(std::span<const std::byte> request,
                                          MessageType expected_reply);

  /// Reads and dispatches one device-pushed message.
  ///
  /// @param expected_reply The message type we are waiting for; when it is read, returns true and writes the view
  ///                       into out_response. Passing std::nullopt means "only drain pushed
  ///                       messages, without waiting for any specific reply".
  [[nodiscard]] Result<bool> PumpOnce(std::optional<MessageType> expected_reply,
                                      std::span<const std::byte>& out_response);

  /// Handles one INDICATE_STATUS.
  void HandleIndicateStatus(std::span<const std::byte> message);

  /// Handles a KEEPALIVE_MSG actively sent by the device -- **KEEPALIVE_CMPLT must be replied**.
  [[nodiscard]] Status HandleDeviceKeepAlive(std::span<const std::byte> message);

  /// Queries an OID. When `fatal` is false, the device returning "unsupported" is not counted as an error.
  [[nodiscard]] Result<std::span<const std::byte>> QueryOid(Oid oid,
                                                            std::uint32_t expected_bytes,
                                                            bool fatal);

  /// Writes an LE32-type OID.
  [[nodiscard]] Status SetOidUint32(Oid oid, std::uint32_t value);

  /// Collects device information (steps 2~4). Failures of non-fatal OIDs are only logged.
  [[nodiscard]] Status CollectDeviceInfo();

  /// Sends HALT_MSG. Best effort, does not return an error.
  void SendHalt() noexcept;

  void TransitionTo(State next);

  /// Allocates the next RequestId.
  ///
  /// Increments from 1 and skips 0: some devices treat 0 as an "invalid ID".
  [[nodiscard]] std::uint32_t NextRequestId() noexcept;

  /// Records "a message was just received from the device", used for the keepalive idle judgment.
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

  /// The packet filter value most recently set successfully. Used when a replay is required after RESET.
  std::uint32_t active_packet_filter_ = 0;

  /// Assembly buffer for request messages. Control messages are small, so one buffer suffices.
  std::vector<std::byte> request_buffer_;

  bool link_connected_ = false;
};

}  // namespace tetherkitnext::rndis
