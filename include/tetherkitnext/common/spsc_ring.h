// Single-producer single-consumer (SPSC) lock-free bounded ring queue.
//
// Why write our own instead of using the ready-made std::queue + mutex:
//   The data path carries 25k~80k frames per second, with one queue in each direction. Locking and unlocking a mutex
//   goes through the kernel futex under contention, at a cost of microseconds per operation; an SPSC lock-free queue's enqueue/dequeue needs only
//   one release store / acquire load, about a few nanoseconds.
//
// This file provides two layers:
//   * SpscCursor -- only responsible for index advancement and memory ordering, indifferent to the storage form.
//   * SpscRing<T> -- a fixed-size element queue, suited to small PODs (completion events, indices, statistics snapshots).
// Variable-length Ethernet frames go through FrameRing (frame_ring.h), which reuses the same SpscCursor.
//
// Correctness is verified by ThreadSanitizer:
//   cmake -B build-tsan -DTETHERKITNEXT_ENABLE_TSAN=ON && ctest --test-dir build-tsan
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "tetherkitnext/common/cache.h"

namespace tetherkitnext {

/// Index cursor of an SPSC queue: manages "how far written / how far read" and the cross-thread memory ordering.
///
/// Uses freely incrementing 64-bit counters rather than the classic "leave one slot empty" approach:
///   * The queue length is directly `write_pos - read_pos`, with no need to handle wrap-around comparisons;
///   * The capacity can be fully used (the classic approach wastes one slot);
///   * A 64-bit counter at 100 Mpps takes 5800 years to wrap, so it can be treated as never wrapping.
///
/// The layout deliberately gives each of the four indices its own cache line:
///   * `write_pos_` is written by the producer and read by the consumer;
///   * `cached_read_pos_` is the producer's private copy of `read_pos_`, used to avoid reading
///     the consumer's cache line on every enqueue (this is the most important optimization of this class -- only when it "looks full"
///     does it actually synchronize once);
///   * `read_pos_` / `cached_write_pos_` are symmetric.
/// If `write_pos_` and `cached_read_pos_` were placed on the same cache line, the consumer reading
/// `write_pos_` would demote that line to Shared, and the producer's subsequent write to `cached_read_pos_` would have to
/// take it back to Exclusive, wasting a round trip of cache coherence.
class SpscCursor {
 public:
  /// `capacity` must be a power of two and greater than 0 (a bit mask is used for modulo).
  explicit SpscCursor(std::size_t capacity) noexcept : capacity_(capacity), mask_(capacity - 1) {}

  [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

  // ---------------------------------------------------------------------------
  // Producer side (only the producer thread may call)
  // ---------------------------------------------------------------------------

  /// Tries to obtain the index of the slot at "current write position + offset". Returns false when the queue is full.
  ///
  /// `offset` is relative to the **not yet published** write position, used for batch writes:
  /// take slots and write with offset = 0,1,2,... in succession, and finally call PublishWrite(n) once.
  ///
  /// Deliberately only reads its own write_pos_ here (relaxed; this cache line is exclusively the producer's),
  /// so taking a slot itself costs almost nothing -- what is really expensive is publishing; see PublishWrite.
  [[nodiscard]] bool TryAcquireWriteAt(std::uint64_t offset, std::size_t& index) noexcept {
    const std::uint64_t position = write_pos_.load(std::memory_order_relaxed) + offset;
    if (position - cached_read_pos_ >= capacity_) [[unlikely]] {
      // The cached read position is stale; synchronize the real value once before deciding. acquire guarantees: when we see the consumer's
      // advanced read_pos_, the consumer's read of that slot has also completed, so it is safe to overwrite.
      cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
      if (position - cached_read_pos_ >= capacity_) {
        return false;
      }
    }
    index = position & mask_;
    return true;
  }

  /// Tries to obtain the next writable slot (equivalent to TryAcquireWriteAt(0, index)).
  [[nodiscard]] bool TryAcquireWrite(std::size_t& index) noexcept {
    return TryAcquireWriteAt(0, index);
  }

  /// Publishes `count` just-written slots, making them visible to the consumer.
  ///
  /// release guarantees: after the consumer reads the new write_pos_ with acquire, it will certainly see all of our
  /// writes to the contents of these slots.
  ///
  /// * **Batch publishing is the most important optimization of this class.** *
  /// This release store makes the cache line holding write_pos_ bounce across cores once, at a measured cost of about
  /// 60 ns. When publishing one at a time it is the per-entry cost; when publishing N at once it is amortized to 60/N:
  ///
  ///     batch=1  -> about 63 ns/entry
  ///     batch=8  -> about 5.5 ns/entry   (about 11x)
  ///     batch=32 -> about 1.9 ns/entry   (about 33x)
  ///
  /// So on the data path publishing **must** be done in batches -- use FrameRing's BatchWriter /
  /// BatchReader, which separate "take slot + write" from "publish", and do
  /// one atomic operation only at the end of the batch.
  void PublishWrite(std::uint64_t count = 1) noexcept {
    write_pos_.store(write_pos_.load(std::memory_order_relaxed) + count,
                     std::memory_order_release);
  }

  /// Remaining writable slots from the producer's point of view (may be conservative, because the cached read position is used).
  [[nodiscard]] std::size_t WritableApprox() noexcept {
    const std::uint64_t write_pos = write_pos_.load(std::memory_order_relaxed);
    cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
    return capacity_ - static_cast<std::size_t>(write_pos - cached_read_pos_);
  }

  // ---------------------------------------------------------------------------
  // Consumer side (only the consumer thread may call)
  // ---------------------------------------------------------------------------

  /// Tries to obtain the index of the slot at "current read position + offset". Returns false when the queue is empty.
  ///
  /// Symmetric to the write side: take a batch with offset = 0,1,2,..., and finally call PublishRead(n) once.
  [[nodiscard]] bool TryAcquireReadAt(std::uint64_t offset, std::size_t& index) noexcept {
    const std::uint64_t position = read_pos_.load(std::memory_order_relaxed) + offset;
    if (position >= cached_write_pos_) [[unlikely]] {
      cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
      if (position >= cached_write_pos_) {
        return false;
      }
    }
    index = position & mask_;
    return true;
  }

  /// Tries to obtain the next readable slot (equivalent to TryAcquireReadAt(0, index)).
  [[nodiscard]] bool TryAcquireRead(std::size_t& index) noexcept {
    return TryAcquireReadAt(0, index);
  }

  /// Releases `count` just-read slots so they can be reused by the producer.
  ///
  /// For the same reason as PublishWrite, batch release amortizes the cost of cross-core cache line bouncing by a factor of N.
  void PublishRead(std::uint64_t count = 1) noexcept {
    read_pos_.store(read_pos_.load(std::memory_order_relaxed) + count,
                    std::memory_order_release);
  }

  /// Readable slots from the consumer's point of view. Use it to get the batch size at once during batch dequeue, reducing synchronization count.
  [[nodiscard]] std::size_t ReadableApprox() noexcept {
    const std::uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
    cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(cached_write_pos_ - read_pos);
  }

  // ---------------------------------------------------------------------------
  // May be called from any thread (for statistics/observation only; the result is an instantaneous snapshot)
  // ---------------------------------------------------------------------------

  /// Approximate number of elements currently in the queue.
  [[nodiscard]] std::size_t SizeSnapshot() const noexcept {
    const std::uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
    const std::uint64_t read_pos = read_pos_.load(std::memory_order_acquire);
    return write_pos >= read_pos ? static_cast<std::size_t>(write_pos - read_pos) : 0;
  }

  /// Total number of enqueues by the producer, for statistics.
  [[nodiscard]] std::uint64_t TotalEnqueued() const noexcept {
    return write_pos_.load(std::memory_order_relaxed);
  }

  /// Total number of dequeues by the consumer, for statistics.
  [[nodiscard]] std::uint64_t TotalDequeued() const noexcept {
    return read_pos_.load(std::memory_order_relaxed);
  }

 private:
  // Read-only shared data gets a line of its own, to keep it from being crowded together with frequently changing indices.
  TETHERKITNEXT_CACHE_ALIGNED const std::size_t capacity_;
  const std::size_t mask_;

  TETHERKITNEXT_CACHE_ALIGNED std::atomic<std::uint64_t> write_pos_{0};
  TETHERKITNEXT_CACHE_ALIGNED std::uint64_t cached_read_pos_{0};
  TETHERKITNEXT_CACHE_ALIGNED std::atomic<std::uint64_t> read_pos_{0};
  TETHERKITNEXT_CACHE_ALIGNED std::uint64_t cached_write_pos_{0};
};

/// SPSC lock-free bounded queue of fixed-size elements.
///
/// Element types must be trivially copyable (POD): the data path does not accept types that may allocate memory or throw.
template <typename T>
  requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class SpscRing {
 public:
  /// `capacity` is rounded up to a power of two.
  explicit SpscRing(std::size_t capacity)
      : cursor_(RoundUpCapacity(capacity)),
        slots_(std::make_unique<T[]>(RoundUpCapacity(capacity))) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;
  ~SpscRing() = default;

  [[nodiscard]] std::size_t Capacity() const noexcept { return cursor_.Capacity(); }

  /// Enqueues. Returns false when the queue is full (the caller decides whether to drop or retry).
  [[nodiscard]] bool TryPush(const T& value) noexcept {
    std::size_t index = 0;
    if (!cursor_.TryAcquireWrite(index)) {
      return false;
    }
    slots_[index] = value;
    cursor_.PublishWrite();
    return true;
  }

  /// Dequeues. Returns false when the queue is empty.
  [[nodiscard]] bool TryPop(T& out) noexcept {
    std::size_t index = 0;
    if (!cursor_.TryAcquireRead(index)) {
      return false;
    }
    out = slots_[index];
    cursor_.PublishRead();
    return true;
  }

  [[nodiscard]] std::size_t SizeSnapshot() const noexcept { return cursor_.SizeSnapshot(); }

  [[nodiscard]] SpscCursor& Cursor() noexcept { return cursor_; }

 private:
  static std::size_t RoundUpCapacity(std::size_t requested) noexcept {
    std::size_t capacity = 1;
    while (capacity < requested) {
      capacity <<= 1U;
    }
    return capacity;
  }

  SpscCursor cursor_;
  std::unique_ptr<T[]> slots_;
};

}  // namespace tetherkitnext
