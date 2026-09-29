// SPSC lock-free ring queue for variable-length Ethernet frames.
//
// Difference from SpscRing<T>: Ethernet frames are variable-length (14~2048 bytes), and we want
// **exactly one memory copy along the entire data path**. So this provides a "reserve slot -> write directly ->
// commit" interface, instead of "assemble elsewhere first, then enqueue":
//
//   Producer (libusb callback thread)         Consumer (BPF write thread)
//   ------------------------------------      -----------------------------------
//   std::byte* dst = ring.BeginWrite();       auto view = ring.BeginRead();
//   memcpy(dst, usb_payload, len);   <- the   ::write(bpf_fd, view.data(), ...);
//   ring.CommitWrite(len);              only  ring.CommitRead();
//                                       copy
//
// Why this one copy is necessary (rather than zero-copy):
//   libusb's bulk IN transfer buffer must be resubmitted as soon as possible to keep the USB pipe saturated; if the
//   buffer is handed to a downstream holder, far more buffers than the "in-flight transfer count" would have to be prepared,
//   and a return mechanism introduced. Measured, a 1500-byte memcpy takes about 40~60 ns on Apple Silicon, while one BPF write()
//   system call is 30~50 times that -- the copy is not the bottleneck at all, and going zero-copy for it is the wrong optimization direction.
//
// Storage layout: `capacity` fixed-size slots, each slot = a cache-line-aligned header (length) + a frame data area.
// Why fixed-size slots rather than a compact arena: an arena has to handle wrap-around splicing across the ring tail, which would turn
// "one write() per frame" into two, making it slower.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "tetherkitnext/common/cache.h"
#include "tetherkitnext/common/spsc_ring.h"

namespace tetherkitnext {

/// Maximum size of an Ethernet frame in bytes (excluding FCS).
///
/// Why 2048: feth's `net.link.fake.max_mtu` is 2048, so the MTU upper bound
/// is 2048; adding the 14-byte Ethernet header would nominally be 2062; but an RNDIS device may negotiate a larger
/// `OID_GEN_MAXIMUM_FRAME_SIZE`, and power-of-two slot alignment makes address computation easier,
/// so 2048 is simply taken as the frame data area capacity; oversized frames are dropped and counted before enqueueing.
inline constexpr std::uint32_t kMaxEthernetFrameBytes = 2048;

/// Minimum legal Ethernet frame (destination MAC 6 + source MAC 6 + EtherType 2).
inline constexpr std::uint32_t kMinEthernetFrameBytes = 14;

/// Read-only frame view pointing into the ring's internal storage, valid until CommitRead.
struct FrameView {
  const std::byte* data = nullptr;
  std::uint32_t length = 0;

  [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return {data, length}; }

  [[nodiscard]] bool Empty() const noexcept { return length == 0; }
};

/// SPSC lock-free bounded queue of variable-length frames.
class FrameRing {
 public:
  /// `capacity_frames` is rounded up to a power of two.
  /// `max_frame_bytes` is the per-frame limit, defaulting to kMaxEthernetFrameBytes.
  explicit FrameRing(std::size_t capacity_frames,
                     std::uint32_t max_frame_bytes = kMaxEthernetFrameBytes)
      : cursor_(RoundUpToPowerOfTwo(capacity_frames)),
        max_frame_bytes_(max_frame_bytes),
        slot_stride_(ComputeSlotStride(max_frame_bytes)),
        storage_(AllocateStorage(RoundUpToPowerOfTwo(capacity_frames),
                                 ComputeSlotStride(max_frame_bytes))) {
    // The storage start is cached after being aligned to a cache line, avoiding recomputing the alignment on every slot access.
    // std::align is used instead of integer modulo: the latter needs uintptr_t <-> pointer conversions, which weaken the
    // compiler's alias analysis (which is what clang-tidy's performance-no-int-to-ptr is hinting at).
    void* base = storage_.get();
    std::size_t space = cursor_.Capacity() * slot_stride_ + kCacheLineSize;
    void* aligned = std::align(kCacheLineSize, cursor_.Capacity() * slot_stride_, base, space);
    assert(aligned != nullptr);
    aligned_base_ = static_cast<std::byte*>(aligned);
  }

  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;
  FrameRing(FrameRing&&) = delete;
  FrameRing& operator=(FrameRing&&) = delete;
  ~FrameRing() = default;

  [[nodiscard]] std::size_t Capacity() const noexcept { return cursor_.Capacity(); }

  [[nodiscard]] std::uint32_t MaxFrameBytes() const noexcept { return max_frame_bytes_; }

  /// Total bytes occupied by the queue, used to print the memory budget at startup.
  [[nodiscard]] std::size_t StorageBytes() const noexcept {
    return cursor_.Capacity() * slot_stride_;
  }

  // ---------------------------------------------------------------------------
  // Producer side
  // ---------------------------------------------------------------------------

  /// Reserves a slot and returns a buffer to which `MaxFrameBytes()` bytes can be written; returns an empty span when the queue is full.
  ///
  /// Must be paired with CommitWrite; BeginWrite must not be called again between the two calls.
  [[nodiscard]] std::span<std::byte> BeginWrite() noexcept {
    if (!cursor_.TryAcquireWrite(pending_write_index_)) [[unlikely]] {
      return {};
    }
    return {FrameDataAt(pending_write_index_), max_frame_bytes_};
  }

  /// Commits the frame just written. `length` must be <= MaxFrameBytes().
  void CommitWrite(std::uint32_t length) noexcept {
    assert(length <= max_frame_bytes_);
    StoreLength(pending_write_index_, length);
    cursor_.PublishWrite();
    NotifyConsumer();
  }

  /// Convenience entry point: copies an entire frame into the queue. Returns true on success, false if the queue is full or the frame is oversized.
  [[nodiscard]] bool TryPush(std::span<const std::byte> frame) noexcept {
    if (frame.size() > max_frame_bytes_) [[unlikely]] {
      return false;
    }
    const std::span<std::byte> dst = BeginWrite();
    if (dst.empty()) [[unlikely]] {
      return false;
    }
    std::memcpy(dst.data(), frame.data(), frame.size());
    CommitWrite(static_cast<std::uint32_t>(frame.size()));
    return true;
  }

  /// Number of free slots from the producer's point of view. Used before unpacking to decide whether the whole batch fits, avoiding half-batch drops.
  [[nodiscard]] std::size_t WritableApprox() noexcept { return cursor_.WritableApprox(); }

  /// Batch write session. **On the data path, always use this instead of per-frame TryPush.**
  ///
  /// One release store makes the cache line holding the write position bounce across cores once (measured about 60 ns).
  /// Publishing per frame makes that a fixed cost per frame; batch publishing amortizes it to 60/N:
  ///     batch=1 about 63 ns/frame, batch=8 about 5.5 ns/frame, batch=32 about 1.9 ns/frame.
  ///
  /// Usage (unpacking the multiple RNDIS packets of one transfer inside a libusb callback):
  /// ```
  /// auto batch = ring.BeginBatchWrite();
  /// while (reader.Next(frame) == ReadOutcome::kFrame) {
  ///   const std::span<std::byte> dst = batch.Begin();
  ///   if (dst.empty()) { break; }          // queue full
  ///   std::memcpy(dst.data(), frame.data(), frame.size());
  ///   batch.Commit(frame.size());
  /// }
  /// // all frames are published at once when batch is destroyed
  /// ```
  class BatchWrite {
   public:
    explicit BatchWrite(FrameRing& ring) noexcept : ring_(&ring) {}

    BatchWrite(const BatchWrite&) = delete;
    BatchWrite& operator=(const BatchWrite&) = delete;
    BatchWrite(BatchWrite&&) = delete;
    BatchWrite& operator=(BatchWrite&&) = delete;

    /// Publishes automatically on destruction. Forgetting an explicit Publish will not lose data.
    ~BatchWrite() { Publish(); }

    /// Reserves the next slot and returns a writable buffer; returns an empty span if the queue is full.
    [[nodiscard]] std::span<std::byte> Begin() noexcept {
      if (!ring_->cursor_.TryAcquireWriteAt(staged_, staged_index_)) [[unlikely]] {
        return {};
      }
      return {ring_->FrameDataAt(staged_index_), ring_->max_frame_bytes_};
    }

    /// Records the length of the frame just written. **Performs no atomic operations** -- this is the key to batching.
    void Commit(std::uint32_t length) noexcept {
      assert(length <= ring_->max_frame_bytes_);
      ring_->StoreLength(staged_index_, length);
      ++staged_;
    }

    /// Convenience entry point: copies an entire frame. Returns true on success.
    [[nodiscard]] bool Push(std::span<const std::byte> frame) noexcept {
      if (frame.size() > ring_->max_frame_bytes_) [[unlikely]] {
        return false;
      }
      const std::span<std::byte> dst = Begin();
      if (dst.empty()) [[unlikely]] {
        return false;
      }
      std::memcpy(dst.data(), frame.data(), frame.size());
      Commit(static_cast<std::uint32_t>(frame.size()));
      return true;
    }

    /// Publishes all staged frames immediately. May be called early; afterwards this session can keep staging new frames.
    void Publish() noexcept {
      if (staged_ != 0) {
        ring_->cursor_.PublishWrite(staged_);
        ring_->NotifyConsumer();
        published_ += staged_;
        staged_ = 0;
      }
    }

    /// Number of frames staged in this session but not yet published.
    [[nodiscard]] std::uint32_t Staged() const noexcept { return staged_; }

    /// Cumulative number of frames published by this session.
    [[nodiscard]] std::uint32_t Published() const noexcept { return published_; }

   private:
    FrameRing* ring_;
    std::uint32_t staged_ = 0;
    std::uint32_t published_ = 0;
    std::size_t staged_index_ = 0;
  };

  [[nodiscard]] BatchWrite BeginBatchWrite() noexcept { return BatchWrite{*this}; }

  // ---------------------------------------------------------------------------
  // Consumer side
  // ---------------------------------------------------------------------------

  /// Takes a read-only view of the frame at the head; returns a view with length == 0 when the queue is empty.
  ///
  /// The view is valid until the next CommitRead. Must be paired with CommitRead.
  [[nodiscard]] FrameView BeginRead() noexcept {
    if (!cursor_.TryAcquireRead(pending_read_index_)) [[unlikely]] {
      return FrameView{.data = nullptr, .length = 0};
    }
    return FrameView{.data = FrameDataAt(pending_read_index_),
                     .length = LoadLength(pending_read_index_)};
  }

  /// Releases the frame at the head.
  void CommitRead() noexcept { cursor_.PublishRead(); }

  /// Number of readable frames from the consumer's point of view. Use it to get the batch size at once during batch consumption.
  [[nodiscard]] std::size_t ReadableApprox() noexcept { return cursor_.ReadableApprox(); }

  /// Batch read session. Symmetric to BatchWrite; performs only one release store at the end of the batch.
  ///
  /// This is exactly the shape the BPF write thread wants: take dozens of frames at once, gather them into one BIOCSBATCHWRITE
  /// batch write, and then release all slots at once.
  ///
  /// Usage:
  /// ```
  /// auto batch = ring.BeginBatchRead();
  /// std::vector<FrameView> views;
  /// while (views.size() < kMaxBatch) {
  ///   const FrameView view = batch.Next();
  ///   if (view.Empty()) { break; }
  ///   views.push_back(view);              // the view is valid until batch is destroyed
  /// }
  /// link.WriteFrames(views);
  /// // all slots are released at once when batch is destroyed
  /// ```
  class BatchRead {
   public:
    explicit BatchRead(FrameRing& ring) noexcept : ring_(&ring) {}

    BatchRead(const BatchRead&) = delete;
    BatchRead& operator=(const BatchRead&) = delete;
    BatchRead(BatchRead&&) = delete;
    BatchRead& operator=(BatchRead&&) = delete;

    /// Releases the taken slots automatically on destruction.
    ~BatchRead() { Release(); }

    /// Takes a read-only view of the next frame; returns a view with length == 0 when there are no more frames.
    ///
    /// The returned view is valid **until** this session's Release() (or destruction) -- because the slot has not yet been
    /// returned to the producer, its content will not be overwritten. This is the precondition for zero-copy batch writes.
    [[nodiscard]] FrameView Next() noexcept {
      std::size_t index = 0;
      if (!ring_->cursor_.TryAcquireReadAt(staged_, index)) [[unlikely]] {
        return FrameView{};
      }
      ++staged_;
      return FrameView{.data = ring_->FrameDataAt(index),
                       .length = ring_->LoadLength(index)};
    }

    /// Releases the taken slots immediately. All previously returned views are invalidated after the call.
    void Release() noexcept {
      if (staged_ != 0) {
        ring_->cursor_.PublishRead(staged_);
        released_ += staged_;
        staged_ = 0;
      }
    }

    /// Number of frames taken but not yet released.
    [[nodiscard]] std::uint32_t Staged() const noexcept { return staged_; }

    /// Cumulative number of frames released by this session.
    [[nodiscard]] std::uint32_t Released() const noexcept { return released_; }

   private:
    FrameRing* ring_;
    std::uint32_t staged_ = 0;
    std::uint32_t released_ = 0;
  };

  [[nodiscard]] BatchRead BeginBatchRead() noexcept { return BatchRead{*this}; }

  // ---------------------------------------------------------------------------
  // Observation
  // ---------------------------------------------------------------------------

  // ---------------------------------------------------------------------------
  // Consumer parking (eventcount).
  //
  // The consumer used to spin on std::this_thread::yield() while the ring was
  // empty. yield() is not a sleep: with no other runnable thread at the same
  // priority it returns immediately, so an idle session pinned one core at
  // ~100% (upstream XiaoMiku01/TetherKit#5). The consumer now parks on a futex
  // (std::atomic::wait -> __ulock_wait) and producers wake it only when it is
  // actually parked, so the hot path pays a single fence + relaxed load.
  //
  // Correctness is the classic Dekker handshake: the consumer publishes
  // `parked = true`, fences, then re-checks the ring; the producer publishes
  // the frame, fences, then checks `parked`. With both seq_cst fences at least
  // one side observes the other, so a wakeup can never be lost.
  // ---------------------------------------------------------------------------

  /// Producer side: wake the consumer if (and only if) it is parked.
  void NotifyConsumer() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (park_.parked.load(std::memory_order_relaxed)) [[unlikely]] {
      Wake();
    }
  }

  /// Unconditionally wake a parked consumer. Used by producers (via
  /// NotifyConsumer) and by the control path to deliver stop/pause requests.
  ///
  /// Callers that want the consumer to observe a flag must store that flag
  /// *before* calling Wake(): the release on the doorbell pairs with the
  /// acquire in WaitForReadable().
  void Wake() noexcept {
    park_.doorbell.fetch_add(1, std::memory_order_release);
    park_.doorbell.notify_one();
  }

  /// Consumer side: block until the ring is non-empty, Wake() is called, or
  /// `should_abort()` returns true. May return spuriously; callers loop.
  template <typename AbortPredicate>
  void WaitForReadable(AbortPredicate&& should_abort) noexcept {
    const std::uint32_t ticket = park_.doorbell.load(std::memory_order_acquire);
    park_.parked.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (cursor_.ReadableApprox() == 0 && !should_abort()) {
      park_.doorbell.wait(ticket, std::memory_order_acquire);
    }
    park_.parked.store(false, std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t SizeSnapshot() const noexcept { return cursor_.SizeSnapshot(); }

  [[nodiscard]] std::uint64_t TotalEnqueued() const noexcept { return cursor_.TotalEnqueued(); }

  [[nodiscard]] std::uint64_t TotalDequeued() const noexcept { return cursor_.TotalDequeued(); }

 private:
  // The two batch session classes need direct access to the cursors and slot addressing, avoiding an extra forwarding layer on the hot path.
  friend class BatchWrite;
  friend class BatchRead;

  /// In-slot layout: [0, 4) stores the length, [kSlotHeaderBytes, ...) stores the frame data.
  ///
  /// Frame data starts at a kCacheLineSize offset, ensuring each frame's start address is cache-line aligned --
  /// both memcpy and the subsequent write() can take the optimal path.
  static constexpr std::size_t kSlotHeaderBytes = kCacheLineSize;

  static std::size_t RoundUpToPowerOfTwo(std::size_t requested) noexcept {
    std::size_t value = 1;
    while (value < requested) {
      value <<= 1U;
    }
    return value;
  }

  /// Slot stride: header + frame data area, rounded up to a cache line, so each slot exclusively owns whole lines.
  static std::size_t ComputeSlotStride(std::uint32_t max_frame_bytes) noexcept {
    const std::size_t raw = kSlotHeaderBytes + max_frame_bytes;
    return ((raw + kCacheLineSize - 1) / kCacheLineSize) * kCacheLineSize;
  }

  static std::unique_ptr<std::byte[]> AllocateStorage(std::size_t capacity, std::size_t stride) {
    // new[] is used instead of aligned_alloc: the alignment of the whole block's start is handled by AlignedBase below,
    // so destruction remains a simple unique_ptr and needs no custom deleter.
    return std::make_unique<std::byte[]>(capacity * stride + kCacheLineSize);
  }

  [[nodiscard]] std::byte* SlotAt(std::size_t index) const noexcept {
    return aligned_base_ + index * slot_stride_;
  }

  [[nodiscard]] std::byte* FrameDataAt(std::size_t index) const noexcept {
    return SlotAt(index) + kSlotHeaderBytes;
  }

  void StoreLength(std::size_t index, std::uint32_t length) noexcept {
    std::memcpy(SlotAt(index), &length, sizeof(length));
  }

  [[nodiscard]] std::uint32_t LoadLength(std::size_t index) const noexcept {
    std::uint32_t length = 0;
    std::memcpy(&length, SlotAt(index), sizeof(length));
    return length;
  }

  SpscCursor cursor_;

  // Read-only after construction; threads on both sides only read, so they can safely share the same cache line.
  std::uint32_t max_frame_bytes_;
  std::size_t slot_stride_;
  std::unique_ptr<std::byte[]> storage_;
  std::byte* aligned_base_ = nullptr;

  // Index of the "reserved but not yet committed" slot: private to the producer and the consumer respectively; they **must** be on different
  // cache lines -- otherwise every frame's BeginWrite/BeginRead would invalidate each other,
  // and the cache-line isolation done earlier for SpscCursor would be wasted.
  TETHERKITNEXT_CACHE_ALIGNED std::size_t pending_write_index_ = 0;
  TETHERKITNEXT_CACHE_ALIGNED std::size_t pending_read_index_ = 0;

  // Park state lives on its own cache line: `parked` is read by the producer
  // on every publish but written only when the consumer goes to sleep.
  struct TETHERKITNEXT_CACHE_ALIGNED ParkState {
    std::atomic<std::uint32_t> doorbell{0};
    std::atomic<bool> parked{false};
  };
  ParkState park_;
};

}  // namespace tetherkitnext
