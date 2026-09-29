// Data-path bridge: connects the RNDIS data channel with the feth/BPF link.
//
// * Threading model (the most important design decision in this file) *
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ RX direction (device -> host)                                       │
//   │                                                                     │
//   │  libusb event thread                    RX injection thread         │
//   │  ───────────────                        ───────────                 │
//   │  handle_events()                        BatchRead: take a batch     │
//   │   └→ bulk IN callback              ┌──► └→ LinkBackend::WriteFrames │
//   │       ├ unpack RNDIS packets       │       (one syscall per batch)  │
//   │       ├ BatchWrite into FrameRing ──┘                               │
//   │       └ resubmit immediately                                        │
//   └─────────────────────────────────────────────────────────────────────┘
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ TX direction (host -> device)                                       │
//   │                                                                     │
//   │  TX extractor thread                                                │
//   │  ───────────                                                        │
//   │  LinkBackend::ReadFrames() (blocking, kernel batches frames)        │
//   │   └→ DataChannel::SendFrames (RNDIS multi-packet, async bulk OUT)   │
//   └─────────────────────────────────────────────────────────────────────┘
//
//   In total there are three data-path threads (plus libusb's own IOKit runloop thread and the state machine's control
//   thread). This machine has 4 performance cores + 6 efficiency cores; three hot threads + libusb's runloop thread exactly
//   fill the performance-core cluster without oversubscribing.
//
//   Why must RX be split into two threads?
//     libusb's transfer callback holds ctx->event_waiters_lock, and doing blocking I/O in the callback would
//     hold up all threads waiting on synchronous transfers (including the state machine thread running the control channel). And BPF's
//     write() is a system call (about 660 ns and up on macOS, longer for batch writes). So the callback only does
//     "unpack + memcpy into the lock-free queue + resubmit immediately", and the real write-out is handed to the injection thread.
//
//   Why does TX need only one thread?
//     BPF's read() itself blocks and aggregates automatically (with BIOCIMMEDIATE=1 it wakes on every arriving packet,
//     and on waking delivers all packets accumulated in the meantime at once), and SendFrames submits **asynchronously** without blocking.
//     So "read a batch -> submit a batch" in the same thread is the optimal shape; one more thread would only add one more
//     cross-core queue handoff.
//
//   Why not merge the three things with a single kqueue event loop?
//     Measured on macOS, one empty kevent() (timeout={0,0}, no events) takes 13.4~13.7 us,
//     20 times more expensive than an ordinary system call. At 25 kpps, probing alone would consume 34% of a single core. kqueue can only be
//     used for genuinely blocking waits, never for polling -- and each of our three paths already has a natural
//     blocking point, so merging brings no benefit.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/common/frame_ring.h"
#include "tetherkitnext/common/stats.h"
#include "tetherkitnext/net/link_backend.h"
#include "tetherkitnext/usb/data_channel.h"

namespace tetherkitnext::core {

/// Bridge layer configuration.
struct BridgeConfig {
  /// RX queue capacity (in frames).
  ///
  /// Capacity derivation: it must buffer the mismatch between "libusb callback bursts" and "BPF write thread scheduling delay".
  /// With a worst-case 10 ms scheduling delay and 1 Gbps full speed (82k pps), about 820 frames are needed; 2048 leaves
  /// a 2.5x margin. Each slot is 2048+128 bytes, so 2048 slots come to about 4.25 MiB.
  std::size_t rx_ring_frames = 2048;

  /// Per-frame limit (including the Ethernet header). Should match feth's MTU + 14.
  std::uint32_t max_frame_bytes = 1518;

  /// The maximum number of frames the RX injection thread gathers before writing out.
  ///
  /// Batch writes amortize "the system-call cost of one write()" to each frame; the measured benefit basically saturates at batch>=8,
  /// but larger batches during bursts can further reduce the number of system calls. 64 balances both:
  /// batches are small when idle (low latency) and automatically grow during bursts (high throughput).
  std::uint32_t rx_write_batch = 64;

  /// How many empty polls the RX injector spins before parking on the ring.
  ///
  /// A short spin covers the "producer is writing the next frame" window
  /// without a futex round-trip; after that the thread blocks (zero idle CPU)
  /// until the USB callback publishes a frame. See FrameRing::WaitForReadable.
  std::uint32_t rx_spin_before_park = 64;

  /// The maximum number of frames per batch submitted to USB in the TX direction.
  std::uint32_t tx_submit_batch = 256;

  /// Statistics report period (milliseconds); 0 means no periodic reports.
  std::uint32_t stats_interval_millis = 0;
};

/// Observable snapshot of the bridge layer runtime.
struct BridgeStats {
  DirectionSnapshot rx;
  DirectionSnapshot tx;
  /// Current depth of the RX queue, used to judge which side is the bottleneck.
  std::size_t rx_queue_depth = 0;
  /// Kernel BPF-side cumulative drops (bs_drop).
  std::uint64_t link_kernel_drops = 0;
  /// Number of times TX entered a wait because the transfer pool had no free slot.
  ///
  /// This counts **events**, not frames: every time SendFrames fails to submit even one frame and the TX thread goes into
  /// a wait, it goes +1 (under sustained pressure at most one more every ~50 ms). It measures "how frequently the TX side hits
  /// the pool limit"; its presence does not mean frames were dropped -- during the wait the frames remain safely in the batch, upstream bursts are absorbed by the
  /// kernel BPF buffer, and a real overflow would show up in link_kernel_drops.
  std::uint64_t tx_backpressure_events = 0;
};

/// Data-path bridge.
///
/// Lifecycle: Create -> Start -> (running) -> Stop. Stop is idempotent, and the destructor calls it as a backstop.
class Bridge {
 public:
  Bridge(usb::DataChannel& data_channel, net::LinkBackend& link, const BridgeConfig& config);

  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;
  Bridge(Bridge&&) = delete;
  Bridge& operator=(Bridge&&) = delete;

  ~Bridge();

  /// Starts the data path.
  [[nodiscard]] Status Start();

  /// Stops the data path.
  ///
  /// The teardown order is deliberate; see the comments in the implementation -- a wrong order loses frames or deadlocks.
  void Stop();

  [[nodiscard]] bool Running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] BridgeStats Snapshot() const;

  /// Counters for the RX direction. The data channel's receive callback writes it directly.
  [[nodiscard]] DirectionCounters& RxCounters() noexcept { return counters_.rx; }

  /// Pauses / resumes data movement.
  ///
  /// Used during an RNDIS soft reset: the device discards all outstanding packets, and continuing to send frames to it during that time
  /// is only a waste, and may hit the window when the device rebuilds its internal state.
  ///
  /// `SetPaused(true)` is **synchronous**: after it returns it is guaranteed that the RX injection thread has stopped at the pause branch,
  /// and will neither touch rx_ring_ nor write frames to the link any more.
  ///
  /// This synchronization is required; a flag alone cannot give the guarantee: the worker may have just passed the flag check and be about
  /// to write out a batch of taken frames, while the caller thinks it has already stopped. That is exactly how one
  /// rx_write_batch (16 frames) once slipped out.
  ///
  /// The TX direction cannot get an equally strong guarantee -- it must keep draining BPF, otherwise the kernel buffer would fill up
  /// and turn into kernel-side drops. During a pause it instead discards what it reads, and re-checks before each submit slice,
  /// so after this function returns at most one already in-transit slice can still finish submitting.
  ///
  /// WARNING: Must not be called from inside the RX/TX worker threads -- it would deadlock waiting on itself. The existing callers are all on the control path
  /// (link state changes, device soft reset), and satisfy this precondition.
  void SetPaused(bool paused) noexcept;

  [[nodiscard]] bool Paused() const noexcept { return paused_.load(std::memory_order_acquire); }

 private:
  /// RX injection thread: takes frames from the FrameRing and writes them to the link in batches.
  void RunReceiveInjector() noexcept;

  /// TX extractor thread: reads frames from the link and submits them to USB in batches.
  void RunTransmitExtractor() noexcept;

  usb::DataChannel* data_channel_;
  net::LinkBackend* link_;
  BridgeConfig config_;

  /// RX queue: libusb event thread (producer) -> RX injection thread (consumer).
  std::unique_ptr<FrameRing> rx_ring_;

  PathCounters counters_;
  std::atomic<std::uint64_t> tx_backpressure_events_{0};
  std::atomic<std::uint64_t> link_kernel_drops_{0};

  std::thread receive_injector_;
  std::thread transmit_extractor_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> paused_{false};

  /// The RX injection thread's acknowledgement bit for paused_, which is what SetPaused(true) waits for.
  ///
  /// **Written exclusively** by the RX injection thread: cleared at the start of each loop iteration, and set after confirming it has stopped.
  /// Clearing on every iteration is key -- otherwise when SetPaused(false) is immediately followed by SetPaused(true),
  /// the latter might read a leftover true from the previous round and return directly, and the guarantee would fail on the spot.
  std::atomic<bool> rx_paused_ack_{false};

  /// The frame view array reused by the RX injection thread, avoiding an allocation per batch.
  std::vector<FrameView> rx_batch_;
};

/// Renders a statistics snapshot into one line of human-readable text.
[[nodiscard]] std::string FormatStatsLine(const BridgeStats& previous, const BridgeStats& current,
                                          double seconds);

}  // namespace tetherkitnext::core
