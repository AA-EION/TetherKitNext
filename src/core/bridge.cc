#include "tetherkitnext/core/bridge.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/common/scheduling.h"

namespace tetherkitnext::core {
namespace {

/// Upper limit of a single wait for a free transfer slot under TX backpressure (milliseconds).
///
/// The value only affects the trade-off between two things: the upper bound of shutdown/pause response latency (the flags are re-checked on every wakeup),
/// and the frequency of needless wakeups during the wait. The slot's actual release period is a few hundred microseconds (the completion
/// time of one bulk OUT), and the vast majority of waits are woken early by the completion callback's notify, almost never waiting the full time.
constexpr std::uint32_t kTxCapacityWaitMillis = 50;

/// Converts a byte count to Mbps.
[[nodiscard]] double ToMegabitsPerSecond(std::uint64_t bytes, double seconds) {
  if (seconds <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(bytes) * 8.0 / 1'000'000.0 / seconds;
}

[[nodiscard]] double ToPacketsPerSecond(std::uint64_t frames, double seconds) {
  if (seconds <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(frames) / seconds;
}

}  // namespace

Bridge::Bridge(usb::DataChannel& data_channel, net::LinkBackend& link,
               const BridgeConfig& config)
    : data_channel_(&data_channel), link_(&link), config_(config) {
  rx_ring_ = std::make_unique<FrameRing>(config_.rx_ring_frames, config_.max_frame_bytes);
  rx_batch_.reserve(config_.rx_write_batch);
}

Bridge::~Bridge() {
  Stop();
}

Status Bridge::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return std::unexpected(Error::Generic(Tr(Msg::kCoreBridgeAlreadyRunning)));
  }
  stop_requested_.store(false, std::memory_order_release);

  // Let the data channel start receiving first -- it pushes frames into rx_ring_.
  // Whether this comes **before** or after starting the injection thread does not matter for ordering (the queue is bounded, and drops
  // and counts when full), but opening receiving first lets the link start receiving as soon as it is ready, dropping a few fewer frames.
  TETHERKITNEXT_RETURN_IF_ERROR(data_channel_->StartReceiving(*rx_ring_, counters_.rx));

  running_.store(true, std::memory_order_release);
  receive_injector_ = std::thread([this] { RunReceiveInjector(); });
  transmit_extractor_ = std::thread([this] { RunTransmitExtractor(); });

  TETHERKITNEXT_INFO_TR(Msg::kCoreDataPathStarted, rx_ring_->Capacity(),
                    rx_ring_->StorageBytes() / 1024, config_.rx_write_batch,
                    config_.tx_submit_batch,
                    Text(link_->SupportsBatchWrite() ? Msg::kCoreBatchWriteAvailable
                                                     : Msg::kCoreBatchWriteUnavailable));
  return Ok();
}

void Bridge::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;  // not running, or already stopped
  }

  // ---------------------------------------------------------------------------
  // Teardown order (a wrong order loses frames or deadlocks)
  //
  //  1. Set the shutdown flag -- the loop conditions of both threads look at it;
  //  2. Interrupt the link's blocking read() -- otherwise the TX extractor thread would stay stuck in it;
  //  3. Join the two data-path threads;
  //  4. Stop the data channel **last**.
  //
  // Step 4 must come last, for two reasons:
  //   * DataChannel::Shutdown() has to wait for all callbacks of in-flight USB transfers to come back, and those callbacks
  //     write into rx_ring_ -- stopping the channel first and then joining would not be wrong; but conversely, if
  //     rx_ring_ were destroyed first it would be a use-after-free. The order here guarantees rx_ring_ stays alive throughout.
  //   * Shutdown() must never be called on the libusb event thread (it would deadlock waiting on itself),
  //     and here is the thread that calls Stop() (the main thread or the control thread), so it is safe.
  // ---------------------------------------------------------------------------
  stop_requested_.store(true, std::memory_order_release);

  // Wake the RX injector if it is parked on an empty ring.
  rx_ring_->Wake();
  link_->Interrupt();

  if (receive_injector_.joinable()) {
    receive_injector_.join();
  }
  if (transmit_extractor_.joinable()) {
    transmit_extractor_.join();
  }

  data_channel_->Shutdown();

  TETHERKITNEXT_INFO_TR(Msg::kCoreDataPathStopped);
}

// =============================================================================
// RX injection thread: FrameRing -> link
// =============================================================================

void Bridge::SetPaused(bool paused) noexcept {
  paused_.store(paused, std::memory_order_release);
  if (!paused) {
    return;
  }

  // Wait for the RX injection thread to confirm it has really stopped. Without this step, after this function returns the worker may still
  // write a batch of already-taken frames onto the link: it may have just passed the flag check above.
  //
  // Wait only when the thread is really running -- before Start() or after Stop() nobody will come to set it,
  // and waiting unconditionally would never return.
  //
  // The injector may be parked on the ring's doorbell, so ring it first —
  // otherwise it would never loop around to observe paused_.
  rx_ring_->Wake();
  while (running_.load(std::memory_order_acquire) &&
         !stop_requested_.load(std::memory_order_acquire) &&
         !rx_paused_ack_.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

void Bridge::RunReceiveInjector() noexcept {
  ConfigureCurrentThread("rx-inject", ThreadRole::kDataPath);
  TETHERKITNEXT_DEBUG_TR(Msg::kCoreRxInjectorStarted);

  std::uint32_t idle_spins = 0;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Withdraw the acknowledgement bit first, then judge whether paused. The order must not be reversed -- leaving the previous round's true would make
    // SetPaused(true) mistakenly think this thread has already stopped, when it is actually about to go take frames.
    rx_paused_ack_.store(false, std::memory_order_release);

    if (paused_.load(std::memory_order_acquire)) {
      // During a reset: do not move data, but do not drop it either -- frames in the queue continue to be sent after resuming.
      //
      // Setting the acknowledgement bit is for SetPaused(true) to see: from here until the next round sees paused_ change back to
      // false, this thread will not touch rx_ring_, so this acknowledgement is trustworthy.
      rx_paused_ack_.store(true, std::memory_order_release);
      // Sleep, don't yield: yield() returns immediately when nothing else is
      // runnable and turns this branch into a 100% CPU spin. Pauses are rare
      // (RNDIS soft reset) and a 1 ms resume latency is irrelevant there.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // Gather a batch and then write. The batch session releases slots all at once only at the end of its scope, so all the views taken in the meantime
    // remain valid -- this is precisely the precondition for "handing zero-copy to one batch write()".
    rx_batch_.clear();
    {
      auto batch = rx_ring_->BeginBatchRead();
      while (rx_batch_.size() < config_.rx_write_batch) {
        const FrameView view = batch.Next();
        if (view.Empty()) {
          break;
        }
        rx_batch_.push_back(view);
      }

      if (rx_batch_.empty()) {
        // Ring is empty. Spin briefly to cover the "producer is mid-write"
        // window, then park until a producer publishes (or stop/pause).
        //
        // This used to std::this_thread::yield() instead of parking. yield()
        // returns immediately when no other thread is runnable, so an idle
        // link burned a full core (upstream XiaoMiku01/TetherKit#5).
        if (++idle_spins < config_.rx_spin_before_park) {
          continue;
        }
        idle_spins = 0;
        rx_ring_->WaitForReadable([this] {
          return stop_requested_.load(std::memory_order_acquire) ||
                 paused_.load(std::memory_order_acquire);
        });
        continue;
      }
      idle_spins = 0;

      const auto result = link_->WriteFrames(rx_batch_);
      if (!result) {
        counters_.rx.AddIoError();
        TETHERKITNEXT_WARN_TR(Msg::kCoreLinkWriteFailed, rx_batch_.size(),
                          result.error().ToString());
        // A write failure does not exit the thread -- it may just be that the interface is temporarily down. The batch session releases the slots as usual
        // (the frames can no longer be delivered, and keeping them would only clog the queue).
      } else {
        if (result->frames_skipped != 0) {
          counters_.rx.AddDroppedOversize(result->frames_skipped);
        }
        const auto written = result->frames_written;
        if (written < rx_batch_.size()) {
          // The link did not take the whole batch (the kernel send queue is full). The remaining frames have already been taken out of the queue,
          // so they can only be dropped and counted -- this is real packet loss and must be made visible to operators.
          counters_.rx.AddDroppedFull(rx_batch_.size() - written);
        }
        // Note: the kernel-side bs_drop is a statistic for the **read** direction, updated by the TX extractor thread
        // from the ReadFrames result, and is not touched here.
      }
    }  // batch session destructor -> one PublishRead(n)
  }

  TETHERKITNEXT_DEBUG_TR(Msg::kCoreRxInjectorExited);
}

// =============================================================================
// TX extractor thread: link -> USB
// =============================================================================

void Bridge::RunTransmitExtractor() noexcept {
  ConfigureCurrentThread("tx-extract", ThreadRole::kDataPath);
  TETHERKITNEXT_DEBUG_TR(Msg::kCoreTxExtractorStarted);

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Blocking read. Under BIOCIMMEDIATE=1 BPF automatically delivers the packets accumulated in the meantime as a batch,
    // so this is naturally "low latency at low rates, large batches at high rates", and we need not batch ourselves.
    const auto batch = link_->ReadFrames();
    if (!batch) {
      counters_.tx.AddIoError();
      TETHERKITNEXT_WARN_TR(Msg::kCoreLinkReadFailed, batch.error().ToString());
      // A read failure may mean the interface was torn down. Wait a bit and retry, to avoid a busy loop flooding the log.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    link_kernel_drops_.store(batch->kernel_drops, std::memory_order_relaxed);

    if (batch->frames.empty()) {
      continue;  // read timeout expired with no packets in the meantime; normal
    }
    if (paused_.load(std::memory_order_acquire)) {
      // During a reset the device discards all outstanding data packets, so sending to it is only a waste.
      counters_.tx.AddDroppedFull(batch->frames.size());
      continue;
    }

    // Submit to USB in batches.
    std::size_t offset = 0;
    while (offset < batch->frames.size()) {
      // Re-check shutdown and pause before every slice: the backpressure wait below lets this loop
      // dwell for a long time, so we cannot rely only on the outer check. Dropping the remaining frames while paused is deliberate (during an RNDIS reset
      // the device discards all outstanding data packets); TX cannot get the strong guarantee RX has (it must keep
      // draining BPF and cannot really stop), but narrowing the leak window to "one slice" is free.
      if (stop_requested_.load(std::memory_order_acquire) ||
          paused_.load(std::memory_order_acquire)) {
        counters_.tx.AddDroppedFull(batch->frames.size() - offset);
        break;
      }

      const std::size_t chunk =
          std::min<std::size_t>(config_.tx_submit_batch, batch->frames.size() - offset);
      const std::span<const FrameView> slice = batch->frames.subspan(offset, chunk);

      const auto sent = data_channel_->SendFrames(slice);
      if (!sent) {
        counters_.tx.AddIoError();
        // The abandoned remaining frames must be counted as drops -- this used to only +1 io_error and break,
        // and the remaining frames entered no counter, vanishing from the statistics out of thin air.
        counters_.tx.AddDroppedFull(batch->frames.size() - offset);
        TETHERKITNEXT_WARN_TR(Msg::kCoreUsbSubmitFailed, chunk, sent.error().ToString());
        break;
      }

      // The bridge layer is the **sole writer** of the TX counters (DirectionCounters uses relaxed
      // load+store, safe only thanks to a single writer). The data channel side only counts errors in the asynchronous completion callbacks,
      // merged in by Snapshot() at read time. The sent frame and byte counts are taken directly from SendOutcome,
      // and skipped illegal-length frames are counted into oversize drops rather than "sent".
      if (sent->sent_frames != 0) {
        counters_.tx.AddBatch(sent->sent_frames, sent->sent_bytes);
      }
      if (sent->skipped != 0) {
        counters_.tx.AddDroppedOversize(sent->skipped);
      }

      if (sent->consumed == 0) {
        // The transfer pool has no free slot -- this is backpressure. **Wait; do not drop.**
        //
        // The old implementation dropped the remaining frames immediately here, on the grounds that "waiting would let the BPF kernel buffer pile up into
        // invisible kernel drops". That premise was wrong: the kernel buffer is 4 MiB by default (about 130 ms of elasticity
        // at 250 Mbps), and its overflow counter bs_drop is brought back on every ReadFrames
        // and shown in the "kernel drops" of the statistics line -- it is not invisible at all.
        // The consequence measured on real hardware: one full-frame slice needs about 26 transfer slots while the pool has only 4,
        // and slots are released within a few hundred microseconds, yet the old code did not wait for even a microsecond, dropping
        // 30%+ of TX traffic here under high load (docs/BENCHMARKS.md "real-device end-to-end measurements").
        //
        // The wait uses a finite timeout: on every wakeup it returns to the top of the loop to re-check the shutdown/pause flags, so shutdown latency
        // has an upper bound. The real overflow backstop is still the kernel buffer -- only when it is full should we drop,
        // and the drop is then visible in the statistics.
        tx_backpressure_events_.fetch_add(1, std::memory_order_relaxed);
        (void)data_channel_->WaitForSendCapacity(kTxCapacityWaitMillis);
        continue;
      }
      offset += sent->consumed;
    }
  }

  TETHERKITNEXT_DEBUG_TR(Msg::kCoreTxExtractorExited);
}

// =============================================================================
// Observation
// =============================================================================

BridgeStats Bridge::Snapshot() const {
  DirectionSnapshot tx = tetherkitnext::Snapshot(counters_.tx);
  // Merge the errors accumulated by the data channel in its asynchronous completion callbacks -- that part is incremented by the libusb event thread,
  // deliberately kept separate from the bridge layer's counters to maintain the invariant "each counter has only one writer".
  tx.io_errors += data_channel_->AsyncSendErrors();

  return BridgeStats{
      .rx = tetherkitnext::Snapshot(counters_.rx),
      .tx = tx,
      .rx_queue_depth = rx_ring_->SizeSnapshot(),
      .link_kernel_drops = link_kernel_drops_.load(std::memory_order_relaxed),
      .tx_backpressure_events = tx_backpressure_events_.load(std::memory_order_relaxed),
  };
}

std::string FormatStatsLine(const BridgeStats& previous, const BridgeStats& current,
                            double seconds) {
  const DirectionSnapshot rx_delta = current.rx - previous.rx;
  const DirectionSnapshot tx_delta = current.tx - previous.tx;

  return Tr(
      Msg::kCoreStatsLine,
      ToPacketsPerSecond(rx_delta.frames, seconds),
      ToMegabitsPerSecond(rx_delta.bytes, seconds), rx_delta.TotalDropped(),
      ToPacketsPerSecond(tx_delta.frames, seconds),
      ToMegabitsPerSecond(tx_delta.bytes, seconds), tx_delta.TotalDropped(),
      current.rx_queue_depth, current.link_kernel_drops - previous.link_kernel_drops,
      current.tx_backpressure_events - previous.tx_backpressure_events);
}

}  // namespace tetherkitnext::core
