#include "tetherkitnext/usb/data_channel.h"

#include <sys/sysctl.h>

#include <cstdlib>
#include <format>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/rndis/packet_codec.h"

namespace tetherkitnext::usb {
namespace {

/// Alignment in bytes of transfer buffers.
///
/// The darwin backend does not provide zero-copy DMA buffers (libusb_dev_mem_alloc returns NULL),
/// so we can only allocate them ourselves. Aligning to the system page size reduces the number of
/// memory segments IOKit needs when building DMA descriptors for the buffer. On Apple Silicon hw.pagesize is 16384.
[[nodiscard]] std::size_t QueryPageSize() {
  std::int64_t value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname("hw.pagesize", &value, &size, nullptr, 0) != 0 || value <= 0) {
    return 16384;  // the actual value on Apple Silicon, used as a backstop
  }
  return static_cast<std::size_t>(value);
}

}  // namespace

// =============================================================================
// Construction and destruction
// =============================================================================

Result<std::unique_ptr<UsbDataChannel>> UsbDataChannel::Create(
    Device& device, const rndis::NegotiatedParameters& parameters,
    const DataChannelConfig& config) {
  auto channel = std::unique_ptr<UsbDataChannel>(new UsbDataChannel());
  channel->device_ = &device;
  channel->parameters_ = parameters;
  channel->config_ = config;

  // The TX buffer must not exceed the MaxTransferSize the device claims -- if it does, the device will reject or truncate.
  channel->tx_transfer_bytes_ =
      std::min(config.tx_transfer_bytes, parameters.device_max_transfer_size);
  if (channel->tx_transfer_bytes_ < rndis::kPacketMsgHeaderBytes + parameters.mtu +
                                        rndis::kEthernetHeaderBytes) {
    return std::unexpected(Error::Generic(Tr(
        Msg::kUsbTxBufferTooSmall, channel->tx_transfer_bytes_,
        rndis::kPacketMsgHeaderBytes + parameters.mtu + rndis::kEthernetHeaderBytes)));
  }

  TETHERKITNEXT_RETURN_IF_ERROR(channel->AllocatePool(channel->rx_pool_, config.rx_transfer_count,
                                                  config.rx_transfer_bytes));
  TETHERKITNEXT_RETURN_IF_ERROR(channel->AllocatePool(channel->tx_pool_, config.tx_transfer_count,
                                                  channel->tx_transfer_bytes_));

  // Queue of free TX slots: initially all free.
  channel->tx_free_slots_ = std::make_unique<SpscRing<std::uint32_t>>(config.tx_transfer_count);
  for (std::uint32_t i = 0; i < config.tx_transfer_count; ++i) {
    if (!channel->tx_free_slots_->TryPush(i)) {
      return std::unexpected(Error::Generic(Tr(Msg::kUsbTxSlotQueueInitFailed)));
    }
  }

  TETHERKITNEXT_INFO_TR(
      Msg::kUsbDataChannelReady,
      config.rx_transfer_count, config.rx_transfer_bytes / 1024,
      config.rx_transfer_count * config.rx_transfer_bytes / 1024, config.tx_transfer_count,
      channel->tx_transfer_bytes_ / 1024, parameters.device_max_transfer_size,
      parameters.max_packets_per_message, parameters.tx_alignment_bytes);

  return channel;
}

UsbDataChannel::~UsbDataChannel() {
  // Shutdown is idempotent; it is called once more here as a backstop, in case the caller forgot.
  Shutdown();
  FreePool(rx_pool_);
  FreePool(tx_pool_);
}

Status UsbDataChannel::AllocatePool(std::vector<Slot>& pool, std::uint32_t count,
                                    std::uint32_t buffer_bytes) {
  const std::size_t alignment = QueryPageSize();
  pool.resize(count);

  for (std::uint32_t i = 0; i < count; ++i) {
    Slot& slot = pool[i];
    slot.owner = this;
    slot.index = i;
    slot.buffer_bytes = buffer_bytes;

    void* raw = nullptr;
    // The length is also rounded up to the alignment boundary, to avoid the tail straddling a page.
    const std::size_t allocation =
        ((buffer_bytes + alignment - 1) / alignment) * alignment;
    if (::posix_memalign(&raw, alignment, allocation) != 0 || raw == nullptr) {
      return std::unexpected(
          Error::Generic(Tr(Msg::kUsbTransferAllocFailed, allocation, alignment)));
    }
    slot.buffer = static_cast<std::byte*>(raw);

    slot.transfer = ::libusb_alloc_transfer(0);
    if (slot.transfer == nullptr) {
      return std::unexpected(Error::Generic(Tr(Msg::kUsbAllocTransferFailed)));
    }
  }
  return Ok();
}

void UsbDataChannel::FreePool(std::vector<Slot>& pool) noexcept {
  for (Slot& slot : pool) {
    if (slot.transfer != nullptr) {
      ::libusb_free_transfer(slot.transfer);
      slot.transfer = nullptr;
    }
    // Memory allocated by posix_memalign must be released with free (not delete).
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(slot.buffer);
    slot.buffer = nullptr;
  }
  pool.clear();
}

// =============================================================================
// Receive path (device -> host)
// =============================================================================

Status UsbDataChannel::StartReceiving(FrameRing& rx_ring, DirectionCounters& rx_counters) {
  rx_ring_ = &rx_ring;
  rx_counters_ = &rx_counters;

  for (Slot& slot : rx_pool_) {
    TETHERKITNEXT_RETURN_IF_ERROR(SubmitReceive(slot));
  }
  TETHERKITNEXT_DEBUG_TR(Msg::kUsbBulkInSubmitted, rx_pool_.size());
  return Ok();
}

Status UsbDataChannel::SubmitReceive(Slot& slot) {
  ::libusb_fill_bulk_transfer(
      slot.transfer, device_->Handle(), device_->BulkInEndpoint(),
      reinterpret_cast<unsigned char*>(slot.buffer), static_cast<int>(slot.buffer_bytes),
      &UsbDataChannel::ReceiveCallbackTrampoline, &slot,
      static_cast<unsigned int>(config_.rx_timeout_millis));

  // **Deliberately not setting LIBUSB_TRANSFER_SHORT_NOT_OK.**
  // A short packet on bulk IN is completely normal semantics (the device ends the transfer with a short packet when it has only half a buffer of data).
  // Setting this flag makes io.c re-judge "COMPLETED with actual_length != requested length" as
  // LIBUSB_TRANSFER_ERROR -- then nearly every transfer would "fail".
  slot.transfer->flags = 0;

  // Increment the in-flight count first and then submit: otherwise the callback might finish running before the increment, making the count underflow.
  outstanding_.fetch_add(1, std::memory_order_acq_rel);

  const int rc = ::libusb_submit_transfer(slot.transfer);
  if (rc != LIBUSB_SUCCESS) {
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    return std::unexpected(Error::FromLibUsb(rc, Tr(Msg::kUsbSubmitBulkInFailed)));
  }
  return Ok();
}

void UsbDataChannel::ReceiveCallbackTrampoline(::libusb_transfer* transfer) {
  auto* slot = static_cast<Slot*>(transfer->user_data);
  slot->owner->OnReceiveComplete(*slot);
}

void UsbDataChannel::OnReceiveComplete(Slot& slot) noexcept {
  // WARNING: This function executes on the libusb **event thread** and holds ctx->event_waiters_lock.
  // Therefore here:
  //   * a synchronous API must never be called (it would return LIBUSB_ERROR_BUSY);
  //   * blocking I/O must never be done (it would hold up all threads waiting on synchronous transfers);
  //   * only do "unpack + memcpy into the lock-free queue + resubmit immediately".
  //   The real BPF write() is handled by another thread.
  const ::libusb_transfer* transfer = slot.transfer;
  bool should_resubmit = true;

  switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED: {
      const auto received = static_cast<std::size_t>(transfer->actual_length);
      if (received > 0 && rx_ring_ != nullptr) {
        // Batch publish: all frames in the whole transfer do only one release store.
        auto batch = rx_ring_->BeginBatchWrite();
        rndis::PacketMessageReader reader(
            std::span<const std::byte>{slot.buffer, received}, rx_ring_->MaxFrameBytes());

        std::span<const std::byte> frame;
        std::uint64_t frame_bytes = 0;
        while (true) {
          const rndis::ReadOutcome outcome = reader.Next(frame);
          if (outcome == rndis::ReadOutcome::kFrame) {
            if (!batch.Push(frame)) [[unlikely]] {
              // The downstream queue is full -- the BPF write thread cannot keep up. Drop and count; must not block here.
              rx_counters_->AddDroppedFull();
              continue;
            }
            frame_bytes += frame.size();
            continue;
          }
          if (outcome == rndis::ReadOutcome::kMalformed) {
            malformed_transfers_.fetch_add(1, std::memory_order_relaxed);
            rx_counters_->AddDroppedMalformed();
            TETHERKITNEXT_TRACE_TR(Msg::kUsbMalformedRndisMessage,
                               rndis::MalformedReasonName(reader.Reason()),
                               reader.FramesDecoded());
          }
          break;
        }
        if (batch.Staged() != 0) {
          const std::uint32_t staged = batch.Staged();
          batch.Publish();
          // NOLINTNEXTLINE(readability-suspicious-call-argument)
          rx_counters_->AddBatch(staged, frame_bytes);
        }
      }
      break;
    }

    case LIBUSB_TRANSFER_STALL:
      // Endpoint halt. Clear it and continue -- libusb_clear_halt goes through a synchronous IOKit call but has **no**
      // usbi_handling_events guard, so calling it inside a callback is safe (the cost is blocking the event thread
      // for tens of microseconds to milliseconds, so it is done only on a real STALL).
      rx_counters_->AddIoError();
      TETHERKITNEXT_WARN_TR(Msg::kUsbBulkInStall);
      if (const auto status = device_->ClearHalt(device_->BulkInEndpoint()); !status) {
        TETHERKITNEXT_ERROR_TR(Msg::kUsbBulkInClearHaltFailed, status.error().ToString());
        should_resubmit = false;
      }
      break;

    case LIBUSB_TRANSFER_CANCELLED:
      // Shutdown path. **Do not** resubmit.
      should_resubmit = false;
      break;

    case LIBUSB_TRANSFER_NO_DEVICE:
      // The device was unplugged. Stop resubmitting, let the in-flight count converge, and leave it to the upper layer's reconnection logic.
      should_resubmit = false;
      TETHERKITNEXT_INFO_TR(Msg::kUsbBulkInDeviceGone);
      break;

    case LIBUSB_TRANSFER_TIMED_OUT:
      // rx_timeout_millis defaults to 0 (infinite), so this should not normally occur. If it occurs, keep submitting.
      rx_counters_->AddIoError();
      break;

    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_OVERFLOW:
    default:
      rx_counters_->AddIoError();
      TETHERKITNEXT_WARN_TR(Msg::kUsbBulkInFailed, static_cast<int>(transfer->status));
      break;
  }

  if (shutting_down_.load(std::memory_order_acquire)) {
    should_resubmit = false;
  }

  if (should_resubmit) {
    // Official guarantee: resubmitting the same transfer directly inside a callback is safe
    // (usbi_handle_transfer_completion has already removed it from the flying list before calling the callback,
    //  and cleared the IN_FLIGHT flag, and does not hold itransfer->lock).
    const int rc = ::libusb_submit_transfer(slot.transfer);
    if (rc == LIBUSB_SUCCESS) {
      return;  // in-flight count stays unchanged: one in, one out
    }
    TETHERKITNEXT_WARN_TR(Msg::kUsbBulkInResubmitFailed, ::libusb_error_name(rc));
  }

  // This transfer leaves flight here. Notifies Shutdown(), which may be waiting for the count to reach zero.
  if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    const std::lock_guard<std::mutex> guard(drain_mutex_);
    drain_condition_.notify_all();
  }
}

// =============================================================================
// Send path (host -> device)
// =============================================================================

bool UsbDataChannel::CanSend() const noexcept {
  return tx_free_slots_ != nullptr && tx_free_slots_->SizeSnapshot() > 0;
}

Result<SendOutcome> UsbDataChannel::SendFrames(std::span<const FrameView> frames) {
  if (frames.empty()) {
    return SendOutcome{};
  }
  if (shutting_down_.load(std::memory_order_acquire)) {
    return std::unexpected(Error::Generic(Tr(Msg::kUsbDataChannelShuttingDown)));
  }

  SendOutcome outcome;

  while (outcome.consumed < frames.size()) {
    std::uint32_t slot_index = 0;
    if (!tx_free_slots_->TryPop(slot_index)) {
      // No free transfer slot -- this is backpressure. Faithfully return the number consumed; the caller should
      // retry the remaining frames after WaitForSendCapacity(), rather than drop them.
      break;
    }
    Slot& slot = tx_pool_[slot_index];

    // Aggregate as many frames as possible into this one transfer.
    rndis::PacketMessageWriter writer(
        std::span<std::byte>{slot.buffer, slot.buffer_bytes},
        rndis::PacketMessageWriter::Limits{
            .max_transfer_bytes = parameters_.device_max_transfer_size,
            .max_messages = parameters_.max_packets_per_message,
            .alignment_bytes = parameters_.tx_alignment_bytes,
        },
        device_->BulkMaxPacketSize());

    std::uint32_t scanned = 0;   // number of frames consumed from the input (including skipped short frames)
    std::uint32_t appended = 0;  // number of frames that actually went into this transfer
    bool append_failed = false;
    while (outcome.consumed + scanned < frames.size()) {
      const FrameView& frame = frames[outcome.consumed + scanned];
      if (frame.length < kMinEthernetFrameBytes) [[unlikely]] {
        // Illegal short frame: skip it and faithfully count it in skipped -- it was not sent.
        ++scanned;
        ++outcome.skipped;
        continue;
      }
      if (!writer.TryAppend(frame.Bytes())) {
        append_failed = true;
        break;  // this batch is full (limited by bytes or packet count), or a single frame is oversized
      }
      ++scanned;
      ++appended;
    }

    if (writer.Empty()) {
      // Not a single frame was put in. Either this whole stretch is skipped short frames, or the first legal frame already exceeds
      // the device's MaxTransferSize -- the latter must be skipped and advanced past, otherwise it would loop forever.
      outcome.consumed += scanned;
      if (append_failed && outcome.consumed < frames.size()) {
        ++outcome.consumed;
        ++outcome.skipped;
      }
      if (!tx_free_slots_->TryPush(slot_index)) {
        TETHERKITNEXT_ERROR_TR(Msg::kUsbReturnTxSlotFailed, slot_index);
      }
      continue;
    }

    const std::uint32_t transfer_bytes = writer.Finish();
    const std::uint64_t payload_bytes = writer.PayloadBytes();
    const std::uint32_t message_count = writer.MessageCount();

    ::libusb_fill_bulk_transfer(
        slot.transfer, device_->Handle(), device_->BulkOutEndpoint(),
        reinterpret_cast<unsigned char*>(slot.buffer), static_cast<int>(transfer_bytes),
        &UsbDataChannel::SendCallbackTrampoline, &slot,
        static_cast<unsigned int>(config_.tx_timeout_millis));
    slot.transfer->flags = 0;

    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    const int rc = ::libusb_submit_transfer(slot.transfer);
    if (rc != LIBUSB_SUCCESS) {
      outstanding_.fetch_sub(1, std::memory_order_acq_rel);
      if (!tx_free_slots_->TryPush(slot_index)) {
        TETHERKITNEXT_ERROR_TR(Msg::kUsbReturnTxSlotFailed, slot_index);
      }
      async_send_errors_.fetch_add(1, std::memory_order_relaxed);
      return std::unexpected(Error::FromLibUsb(rc, Tr(Msg::kUsbSubmitBulkOutFailed)));
    }

    // Frame counts and byte counts are tallied by the **bridge layer** (the sole writer of the TX counters). Here we only record
    // the aggregation effect for diagnostics.
    TETHERKITNEXT_TRACE_TR(Msg::kUsbBulkOutSubmitted, message_count, payload_bytes, transfer_bytes);
    outcome.consumed += scanned;
    outcome.sent_frames += appended;
    outcome.sent_bytes += payload_bytes;
  }

  return outcome;
}

bool UsbDataChannel::WaitForSendCapacity(std::uint32_t timeout_millis) {
  std::unique_lock<std::mutex> lock(send_capacity_mutex_);
  return send_capacity_cv_.wait_for(lock, std::chrono::milliseconds(timeout_millis), [this] {
    return shutting_down_.load(std::memory_order_acquire) ||
           tx_free_slots_->SizeSnapshot() > 0;
  });
}

void UsbDataChannel::SendCallbackTrampoline(::libusb_transfer* transfer) {
  auto* slot = static_cast<Slot*>(transfer->user_data);
  slot->owner->OnSendComplete(*slot);
}

void UsbDataChannel::OnSendComplete(Slot& slot) noexcept {
  // Also on the libusb event thread, and likewise must not block.
  switch (slot.transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
      break;

    case LIBUSB_TRANSFER_STALL:
      async_send_errors_.fetch_add(1, std::memory_order_relaxed);
      TETHERKITNEXT_WARN_TR(Msg::kUsbBulkOutStall);
      if (const auto status = device_->ClearHalt(device_->BulkOutEndpoint()); !status) {
        TETHERKITNEXT_ERROR_TR(Msg::kUsbBulkOutClearHaltFailed, status.error().ToString());
      }
      break;

    case LIBUSB_TRANSFER_CANCELLED:
    case LIBUSB_TRANSFER_NO_DEVICE:
      break;

    default:
      async_send_errors_.fetch_add(1, std::memory_order_relaxed);
      TETHERKITNEXT_WARN_TR(Msg::kUsbBulkOutFailed, static_cast<int>(slot.transfer->status));
      break;
  }

  // Return the slot. Not returning it at shutdown does not matter -- nobody will take from it anyway.
  if (!shutting_down_.load(std::memory_order_acquire)) {
    if (!tx_free_slots_->TryPush(slot.index)) {
      TETHERKITNEXT_ERROR_TR(Msg::kUsbReturnTxSlotFailed, slot.index);
    }
    // Wake the TX thread that may be waiting for a slot in WaitForSendCapacity. Empty-critical-section idiom:
    // taking the lock guarantees the waiter will not miss this notification in the window between "checked the predicate" and "not yet asleep".
    { const std::lock_guard<std::mutex> guard(send_capacity_mutex_); }
    send_capacity_cv_.notify_one();
  }

  if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    const std::lock_guard<std::mutex> guard(drain_mutex_);
    drain_condition_.notify_all();
  }
}

// =============================================================================
// Teardown
// =============================================================================

void UsbDataChannel::Shutdown() {
  if (shutdown_complete_) {
    return;
  }
  // WARNING: This function **must never** be called from the libusb event thread: below it waits for the in-flight count to reach zero,
  // and the callbacks that decrement it run exactly on the event thread -- waiting there means waiting on oneself, an inevitable deadlock.
  // The correct callers are the control thread or the main thread.

  // 1. Set the shutdown flag. Callbacks that see it no longer resubmit, and the in-flight count starts converging naturally.
  //    Also wake threads that may still be waiting for a slot in WaitForSendCapacity (under the normal teardown order
  //    the TX thread has already been joined by now, so this is a defensive backstop -- for example when tests drive the channel directly).
  shutting_down_.store(true, std::memory_order_release);
  { const std::lock_guard<std::mutex> guard(send_capacity_mutex_); }
  send_capacity_cv_.notify_all();

  // 2. Cancel the in-flight transfers.
  //
  // On darwin libusb_cancel_transfer is AbortPipe, which cancels **all** in-flight transfers on **that endpoint**,
  // so in fact each endpoint needs to be called only once; calling one by one here is an idempotent and clearer way to write it.
  // LIBUSB_ERROR_NOT_FOUND means it was not in flight to begin with, which is a normal situation.
  for (Slot& slot : rx_pool_) {
    if (slot.transfer != nullptr) {
      const int rc = ::libusb_cancel_transfer(slot.transfer);
      if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
        TETHERKITNEXT_DEBUG_TR(Msg::kUsbCancelBulkInReturned, ::libusb_error_name(rc));
      }
    }
  }
  for (Slot& slot : tx_pool_) {
    if (slot.transfer != nullptr) {
      const int rc = ::libusb_cancel_transfer(slot.transfer);
      if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
        TETHERKITNEXT_DEBUG_TR(Msg::kUsbCancelBulkOutReturned, ::libusb_error_name(rc));
      }
    }
  }

  // 3. Wait for the in-flight count to reach zero. **This step is the key to avoiding use-after-free** --
  //    it must wait until every callback has finished running before transfers and buffers can be freed.
  {
    constexpr auto kDrainTimeout = std::chrono::seconds(5);
    std::unique_lock<std::mutex> lock(drain_mutex_);
    const bool drained = drain_condition_.wait_for(lock, kDrainTimeout, [this] {
      return outstanding_.load(std::memory_order_acquire) == 0;
    });
    if (!drained) {
      // Timed out. Freeing the buffers now is dangerous (callbacks may still access them), so **deliberately leak** --
      // leaking a few hundred KB is far better than a use-after-free crash.
      const std::uint32_t stuck = outstanding_.load(std::memory_order_acquire);
      TETHERKITNEXT_ERROR_TR(Msg::kUsbTransferReclaimTimeout, stuck,
                         (stuck * config_.rx_transfer_bytes) / 1024);
      // Null out the transfer pointers so FreePool skips them.
      for (Slot& slot : rx_pool_) {
        slot.transfer = nullptr;
        slot.buffer = nullptr;
      }
      for (Slot& slot : tx_pool_) {
        slot.transfer = nullptr;
        slot.buffer = nullptr;
      }
    }
  }

  shutdown_complete_ = true;
  TETHERKITNEXT_DEBUG_TR(Msg::kUsbDataChannelStopped);
}

}  // namespace tetherkitnext::usb
