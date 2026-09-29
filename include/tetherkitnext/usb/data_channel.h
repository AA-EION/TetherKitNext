// RNDIS data channel: asynchronous bulk IN / OUT transfer pools.
//
// * Teardown algorithm free of use-after-free (the most important part of this file) *
//
//   libusb_close **does not** reclaim in-flight transfers for you. For transfers still in flight, do_close()
//   only does list_del + nulls transfer->dev_handle, then prints a
//   "Device handle closed while transfer was still being processed" log --
//   **it neither calls your callback nor frees memory**. And afterwards the darwin backend's close does
//   USBInterfaceClose, which triggers an IOKit abort, and darwin_async_io_callback still runs on libusb's
//   internal thread and calls usbi_signal_transfer_completion(). If by then you have already
//   done libusb_free_transfer, it is a use-after-free.
//
//   The correct order (this is what Shutdown() implements):
//     1. Set the shutdown flag -- callbacks that see it no longer resubmit, letting the in-flight count converge naturally;
//     2. Call libusb_cancel_transfer on the in-flight transfers;
//     3. **Wait until the in-flight count reaches zero** (every callback has come back);
//     4. Only then libusb_free_transfer + release the buffers;
//     5. Only afterwards may the caller destroy the Device (release_interface -> close).
//
//   WARNING: step 3 **must never wait on the libusb event thread** -- callbacks run exactly on that thread,
//      and waiting there means waiting on oneself, an inevitable deadlock. So Shutdown() may only be called from the control thread or the main thread;
//      the code reminds you doubly, with comments and assertions.
//
//   About darwin's cancel granularity: libusb_cancel_transfer on darwin is
//   darwin_abort_transfers -> **AbortPipe(pipeRef)**, i.e. canceling one transfer
//   cancels **all** in-flight transfers on **that endpoint** (all come back as kIOReturnAborted ->
//   LIBUSB_TRANSFER_CANCELLED). So it would actually suffice to call it once per endpoint,
//   but calling it one by one is idempotent and clearer, so this micro-optimization is not done.
//
// * Why "fewer and larger, not more and smaller" *
//
//   The darwin backend calls darwin_get_pipe_properties before **every** submit_bulk_transfer,
//   which on IOUSBInterfaceInterface >= 550 amounts to two IOKit user-client calls
//   (GetPipePropertiesV3 + GetEndpointPropertiesV3). That is, on macOS every
//   libusb_submit_transfer costs at least 3 IOKit round trips, not 1.
//   Conversely, the darwin backend has **no length limit and does no fragmentation** for a single bulk transfer
//   (only the Linux backend has the MAX_BULK_BUFFER_LENGTH = 16384 URB splitting).
//   -> Conclusion: use fewer, larger transfers.
#pragma once

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/common/frame_ring.h"
#include "tetherkitnext/common/spsc_ring.h"
#include "tetherkitnext/common/stats.h"
#include "tetherkitnext/rndis/messages.h"
#include "tetherkitnext/usb/device.h"

namespace tetherkitnext::usb {

/// Configuration of the data channel.
struct DataChannelConfig {
  /// Number of concurrent in-flight bulk IN transfers.
  ///
  /// 8 x 16 KiB = 128 KiB in flight. The rule of thumb from research is that a "total in flight >= 256 KiB" is needed
  /// to keep the controller queue from running dry at USB 2.0 high speed (53.2 MB/s cap) -- at 50 MB/s,
  /// a 1 ms gap wastes 50 KB. So the default is 16 x 16 KiB = 256 KiB.
  std::uint32_t rx_transfer_count = 16;

  /// Bytes per bulk IN buffer.
  ///
  /// Must be >= the MaxTransferSize we claim in INITIALIZE_MSG, otherwise the large transfers the device aggregates
  /// would overflow / be truncated. At runtime this value is overridden with the negotiation result.
  std::uint32_t rx_transfer_bytes = 16 * 1024;

  /// Number of concurrent in-flight bulk OUT transfers.
  ///
  /// The TX side does not need as many as RX: frames are batched up and sent by us proactively, and 4 are enough to make
  /// "assembling the next batch" and "the previous batch in flight" overlap.
  std::uint32_t tx_transfer_count = 4;

  /// Bytes per bulk OUT buffer. Overridden at runtime by the negotiated device MaxTransferSize.
  std::uint32_t tx_transfer_bytes = 16 * 1024;

  /// Timeout of bulk IN (milliseconds).
  ///
  /// **Must be 0 (infinite)**: the RNDIS data channel may be idle for long periods (the user is not transferring data),
  /// and with a timeout set it would keep timing out and resubmitting, each resubmit costing 3 IOKit round trips, purely wasteful.
  /// darwin passes timeout to IOKit as both noDataTimeout and completionTimeout,
  /// and 0 means wait forever.
  std::uint32_t rx_timeout_millis = 0;

  /// Timeout of bulk OUT (milliseconds). A timeout on TX is reasonable -- we need to notice when the device gets stuck.
  std::uint32_t tx_timeout_millis = 5'000;
};

/// Result of one SendFrames.
///
/// Why not return just a single number: it once returned only "number of frames sent", and as a result two kinds of skipped frames
/// (those exceeding the device's MaxTransferSize, and those shorter than an Ethernet header) were counted as "sent" --
/// the comment claimed "the caller infers drops from the return value", yet the caller had no way to infer it.
/// Only by separating "how many consumed", "how many actually sent" and "how many skipped" can the books balance.
struct SendOutcome {
  /// The caller should advance its offset by this many frames (including skipped ones). 0 means the transfer pool is full and not a single frame got in.
  std::uint32_t consumed = 0;
  /// Number of frames that actually entered bulk OUT transfers.
  std::uint32_t sent_frames = 0;
  /// Total payload bytes of the above frames (excluding RNDIS headers and padding).
  std::uint64_t sent_bytes = 0;
  /// Number of frames skipped for illegal length (oversized / shorter than an Ethernet header). Always equals consumed - sent_frames.
  std::uint32_t skipped = 0;
};

/// Abstraction of the data channel.
///
/// The abstraction granularity is the **batch**, not the frame: one virtual call handles dozens to hundreds of frames, and the amortized per-frame overhead is far below
/// 1 ns. This is deliberate -- one virtual call per frame would be unacceptable.
class DataChannel {
 public:
  DataChannel() = default;
  DataChannel(const DataChannel&) = delete;
  DataChannel& operator=(const DataChannel&) = delete;
  DataChannel(DataChannel&&) = delete;
  DataChannel& operator=(DataChannel&&) = delete;
  virtual ~DataChannel() = default;

  /// Starts receiving: pushes decoded Ethernet frames into `rx_ring`.
  ///
  /// After the call, receiving keeps running until Shutdown().
  [[nodiscard]] virtual Status StartReceiving(FrameRing& rx_ring,
                                              DirectionCounters& rx_counters) = 0;

  /// Aggregates a batch of Ethernet frames into RNDIS messages and sends them to the device. **Does not block.**
  ///
  /// `consumed < frames.size()` means the transfer pool temporarily has no free slot (backpressure).
  /// The caller should keep the remaining frames and use WaitForSendCapacity() to wait for a free slot before retrying --
  /// **do not drop them**: upstream (the kernel BPF buffer) is the elastic queue that should absorb bursts.
  [[nodiscard]] virtual Result<SendOutcome> SendFrames(std::span<const FrameView> frames) = 0;

  /// Waits for a free transfer slot to appear in the TX direction.
  ///
  /// Returning true means "worth another SendFrames attempt": a slot has freed up, or the channel has entered
  /// shutdown (a retry would then get an error, and the caller will exit by itself). false means the wait timed out with still no capacity.
  ///
  /// May only be used by the same calling thread as SendFrames (the TX extractor thread). Implementations must guarantee
  /// that Shutdown() can wake a waiting thread -- but the caller should not rely on this for shutdown either;
  /// it should use a finite timeout and check the shutdown flag itself after each wakeup.
  [[nodiscard]] virtual bool WaitForSendCapacity(std::uint32_t timeout_millis) = 0;

  /// Stops and reclaims all in-flight transfers.
  ///
  /// **Must not be called from the libusb event thread** (it would deadlock; see the explanation in the file header).
  virtual void Shutdown() = 0;

  /// Whether TX currently has a free transfer slot available.
  [[nodiscard]] virtual bool CanSend() const noexcept = 0;

  /// Maximum bytes a single transfer can hold (TX direction).
  [[nodiscard]] virtual std::uint32_t MaxTransferBytes() const noexcept = 0;

  /// Number of I/O errors accumulated in the asynchronous send completion callbacks (STALL, transfer failures, etc.).
  ///
  /// Why this separate interface exists, rather than having the data channel write directly to the bridge layer's TX counters:
  /// the contract of DirectionCounters is "**only one thread** may call Add*" (it uses relaxed
  /// load+store rather than fetch_add, which is safe only thanks to a single writer). And the TX direction has two writers --
  /// the submit side is on the TX extractor thread and the error side is on the libusb event thread. Forcing them into the same counter breaks
  /// this invariant (TSan would report it too).
  /// So it is split into two separately single-writer counts, merged by Bridge::Snapshot() **at read time**.
  [[nodiscard]] virtual std::uint64_t AsyncSendErrors() const noexcept = 0;
};

/// Data channel implementation based on libusb's asynchronous API.
class UsbDataChannel final : public DataChannel {
 public:
  /// @param device      The device with interfaces already claimed
  /// @param parameters  RNDIS negotiation result (determines aggregation limit and alignment)
  /// @param config      Transfer pool parameters
  [[nodiscard]] static Result<std::unique_ptr<UsbDataChannel>> Create(
      Device& device, const rndis::NegotiatedParameters& parameters,
      const DataChannelConfig& config);

  // Copy and move are already deleted in the base class DataChannel; they are restated explicitly here to satisfy static analysis.
  UsbDataChannel(const UsbDataChannel&) = delete;
  UsbDataChannel& operator=(const UsbDataChannel&) = delete;
  UsbDataChannel(UsbDataChannel&&) = delete;
  UsbDataChannel& operator=(UsbDataChannel&&) = delete;
  ~UsbDataChannel() override;

  [[nodiscard]] Status StartReceiving(FrameRing& rx_ring,
                                      DirectionCounters& rx_counters) override;
  [[nodiscard]] Result<SendOutcome> SendFrames(std::span<const FrameView> frames) override;
  [[nodiscard]] bool WaitForSendCapacity(std::uint32_t timeout_millis) override;
  void Shutdown() override;

  [[nodiscard]] bool CanSend() const noexcept override;

  [[nodiscard]] std::uint32_t MaxTransferBytes() const noexcept override {
    return tx_transfer_bytes_;
  }

  [[nodiscard]] std::uint64_t AsyncSendErrors() const noexcept override {
    return async_send_errors_.load(std::memory_order_relaxed);
  }

  /// Cumulative number of transfers dropped because of malformed RNDIS messages, used to diagnose device bugs.
  [[nodiscard]] std::uint64_t MalformedTransfers() const noexcept {
    return malformed_transfers_.load(std::memory_order_relaxed);
  }

 private:
  UsbDataChannel() = default;

  /// One transfer in the pool and its buffer.
  struct Slot {
    ::libusb_transfer* transfer = nullptr;
    std::byte* buffer = nullptr;
    std::uint32_t buffer_bytes = 0;
    UsbDataChannel* owner = nullptr;
    std::uint32_t index = 0;
  };

  // ---- libusb callbacks (static trampoline -> member function) ----
  static void ReceiveCallbackTrampoline(::libusb_transfer* transfer);
  static void SendCallbackTrampoline(::libusb_transfer* transfer);

  void OnReceiveComplete(Slot& slot) noexcept;
  void OnSendComplete(Slot& slot) noexcept;

  /// Allocates a pool. Buffers are page-aligned (darwin has no zero-copy DMA buffers,
  /// libusb_dev_mem_alloc returns NULL, so we can only posix_memalign ourselves; aligning to hw.pagesize
  /// reduces fragmentation when IOKit builds DMA descriptors).
  [[nodiscard]] Status AllocatePool(std::vector<Slot>& pool, std::uint32_t count,
                                    std::uint32_t buffer_bytes);

  static void FreePool(std::vector<Slot>& pool) noexcept;

  /// Submits one RX transfer.
  [[nodiscard]] Status SubmitReceive(Slot& slot);

  Device* device_ = nullptr;
  rndis::NegotiatedParameters parameters_{};
  DataChannelConfig config_{};

  std::uint32_t tx_transfer_bytes_ = 0;

  std::vector<Slot> rx_pool_;
  std::vector<Slot> tx_pool_;

  /// Index queue of free TX slots.
  ///
  /// Producer = the libusb event thread (the send completion callback returns slots),
  /// consumer = the BPF read thread (takes slots to assemble the next batch). Exactly SPSC.
  std::unique_ptr<SpscRing<std::uint32_t>> tx_free_slots_;

  /// Receive target. Non-null after StartReceiving.
  FrameRing* rx_ring_ = nullptr;
  DirectionCounters* rx_counters_ = nullptr;

  /// I/O errors accumulated in the asynchronous send completion callbacks. **Incremented only by the libusb event thread**,
  /// kept separate from the bridge layer's TX counters; see the explanation of AsyncSendErrors().
  std::atomic<std::uint64_t> async_send_errors_{0};

  /// In-flight transfer count. Callbacks decrement it; Shutdown waits for it to reach zero.
  std::atomic<std::uint32_t> outstanding_{0};
  /// Shutdown flag. Callbacks that see it no longer resubmit.
  std::atomic<bool> shutting_down_{false};
  /// Whether teardown has completed (Shutdown is idempotent).
  bool shutdown_complete_ = false;

  std::atomic<std::uint64_t> malformed_transfers_{0};

  /// Waits for the in-flight count to reach zero.
  std::mutex drain_mutex_;
  std::condition_variable drain_condition_;

  /// Waiting for a free TX slot.
  ///
  /// The source of truth of the predicate is tx_free_slots_ (a lock-free SPSC ring); this mutex + condition variable pair is only a
  /// wakeup mechanism: after the completion callback returns a slot it takes the lock and then notifies (the empty-critical-section idiom), guaranteeing that
  /// the waiter either sees the new slot in the predicate check or has already entered wait and receives the notification, so no wakeup is missed.
  /// The completion callback is on the libusb event thread, and this lock is held only momentarily, so it does not amount to "callback blocking".
  std::mutex send_capacity_mutex_;
  std::condition_variable send_capacity_cv_;
};

}  // namespace tetherkitnext::usb
