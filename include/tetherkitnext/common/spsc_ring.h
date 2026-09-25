// 单生产者单消费者（SPSC）无锁有界环形队列。
//
// 为什么要自己写而不用现成的 std::queue + mutex：
//   数据路径上每秒要过 25k~80k 个帧，两个方向各一条队列。互斥锁的加解锁在
//   争用时会走内核 futex，单次代价可达微秒级；SPSC 无锁队列的入队/出队只需
//   一次 release store / acquire load，约几纳秒。
//
// 本文件提供两层：
//   * SpscCursor —— 只负责索引推进与内存序，不关心存储形态。
//   * SpscRing<T> —— 定长元素队列，适合小 POD（完成事件、索引、统计快照）。
// 变长以太帧走 FrameRing（frame_ring.h），它复用同一个 SpscCursor。
//
// 正确性由 ThreadSanitizer 验证：
//   cmake -B build-tsan -DTETHERKITNEXT_ENABLE_TSAN=ON && ctest --test-dir build-tsan
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "tetherkitnext/common/cache.h"

namespace tetherkitnext {

/// SPSC 队列的索引游标：管理「写到哪了 / 读到哪了」以及跨线程的内存序。
///
/// 使用自由递增的 64 位计数器而非「留一个空槽」的经典写法：
///   * 队列长度直接是 `write_pos - read_pos`，无需处理回绕比较；
///   * 容量可以被完整利用（经典写法会浪费一个槽）；
///   * 64 位计数器以 100 Mpps 计需要 5800 年才回绕，可以当作永不回绕。
///
/// 布局刻意让四个索引各占一条缓存行：
///   * `write_pos_` 由生产者写、消费者读；
///   * `cached_read_pos_` 是生产者私有的 `read_pos_` 副本，用来避免每次入队都去
///     读消费者的缓存行（这是本类最重要的优化 —— 只在「看起来满了」时才真正
///     去同步一次）；
///   * `read_pos_` / `cached_write_pos_` 对称。
/// 如果把 `write_pos_` 与 `cached_read_pos_` 放在同一条缓存行上，消费者读
/// `write_pos_` 会把该行降级为 Shared，生产者随后写 `cached_read_pos_` 又要
/// 重新取回 Exclusive，白白多一轮缓存一致性往返。
class SpscCursor {
 public:
  /// `capacity` 必须是 2 的幂且大于 0（用位掩码取模）。
  explicit SpscCursor(std::size_t capacity) noexcept : capacity_(capacity), mask_(capacity - 1) {}

  [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

  // ---------------------------------------------------------------------------
  // 生产者侧（只允许生产者线程调用）
  // ---------------------------------------------------------------------------

  /// 尝试取得「当前写位置 + offset」处槽位的下标。队列满时返回 false。
  ///
  /// `offset` 是相对于**尚未发布**的写位置的偏移，用于批量写：
  /// 连续用 offset = 0,1,2,... 取槽位、写入，最后调一次 PublishWrite(n)。
  ///
  /// 这里刻意只读自己的 write_pos_（relaxed，本缓存行归生产者独占），
  /// 因此取槽位本身几乎零成本 —— 真正贵的是发布，见 PublishWrite。
  [[nodiscard]] bool TryAcquireWriteAt(std::uint64_t offset, std::size_t& index) noexcept {
    const std::uint64_t position = write_pos_.load(std::memory_order_relaxed) + offset;
    if (position - cached_read_pos_ >= capacity_) [[unlikely]] {
      // 缓存的读位置过旧，同步一次真实值再判断。acquire 保证：看到消费者
      // 推进后的 read_pos_ 时，消费者对该槽位的读取也已完成，可以安全覆写。
      cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
      if (position - cached_read_pos_ >= capacity_) {
        return false;
      }
    }
    index = position & mask_;
    return true;
  }

  /// 尝试取得下一个可写槽位（等价于 TryAcquireWriteAt(0, index)）。
  [[nodiscard]] bool TryAcquireWrite(std::size_t& index) noexcept {
    return TryAcquireWriteAt(0, index);
  }

  /// 发布 `count` 个刚写好的槽位，使其对消费者可见。
  ///
  /// release 保证：消费者以 acquire 读到新的 write_pos_ 后，一定能看到我们
  /// 对这些槽位内容的全部写入。
  ///
  /// ★ **批量发布是本类最重要的优化。** ★
  /// 这条 release store 会让 write_pos_ 所在的缓存行跨核弹一次，实测成本约
  /// 60 ns。逐条发布时它就是每条目的成本；而一次发布 N 条时被摊薄成 60/N：
  ///
  ///     batch=1  → 约 63 ns/条目
  ///     batch=8  → 约 5.5 ns/条目   （约 11 倍）
  ///     batch=32 → 约 1.9 ns/条目   （约 33 倍）
  ///
  /// 所以数据路径上**必须**成批发布 —— 用 FrameRing 的 BatchWriter /
  /// BatchReader，它们把「取槽位 + 写入」和「发布」分开，只在批次结束时
  /// 做一次原子操作。
  void PublishWrite(std::uint64_t count = 1) noexcept {
    write_pos_.store(write_pos_.load(std::memory_order_relaxed) + count,
                     std::memory_order_release);
  }

  /// 生产者视角的剩余可写槽数（可能偏保守，因为用的是缓存的读位置）。
  [[nodiscard]] std::size_t WritableApprox() noexcept {
    const std::uint64_t write_pos = write_pos_.load(std::memory_order_relaxed);
    cached_read_pos_ = read_pos_.load(std::memory_order_acquire);
    return capacity_ - static_cast<std::size_t>(write_pos - cached_read_pos_);
  }

  // ---------------------------------------------------------------------------
  // 消费者侧（只允许消费者线程调用）
  // ---------------------------------------------------------------------------

  /// 尝试取得「当前读位置 + offset」处槽位的下标。队列空时返回 false。
  ///
  /// 与写侧对称：用 offset = 0,1,2,... 批量取出，最后调一次 PublishRead(n)。
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

  /// 尝试取得下一个可读槽位（等价于 TryAcquireReadAt(0, index)）。
  [[nodiscard]] bool TryAcquireRead(std::size_t& index) noexcept {
    return TryAcquireReadAt(0, index);
  }

  /// 释放 `count` 个刚读完的槽位，使其可被生产者复用。
  ///
  /// 与 PublishWrite 同理，批量释放能把跨核缓存行弹跳的成本摊薄 N 倍。
  void PublishRead(std::uint64_t count = 1) noexcept {
    read_pos_.store(read_pos_.load(std::memory_order_relaxed) + count,
                    std::memory_order_release);
  }

  /// 消费者视角的可读槽数。批量出队时用它一次拿到批大小，减少同步次数。
  [[nodiscard]] std::size_t ReadableApprox() noexcept {
    const std::uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
    cached_write_pos_ = write_pos_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(cached_write_pos_ - read_pos);
  }

  // ---------------------------------------------------------------------------
  // 任意线程可调用（仅用于统计/观测，结果是瞬时快照）
  // ---------------------------------------------------------------------------

  /// 队列中当前元素个数的近似值。
  [[nodiscard]] std::size_t SizeSnapshot() const noexcept {
    const std::uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
    const std::uint64_t read_pos = read_pos_.load(std::memory_order_acquire);
    return write_pos >= read_pos ? static_cast<std::size_t>(write_pos - read_pos) : 0;
  }

  /// 生产者累计入队总数，用于统计。
  [[nodiscard]] std::uint64_t TotalEnqueued() const noexcept {
    return write_pos_.load(std::memory_order_relaxed);
  }

  /// 消费者累计出队总数，用于统计。
  [[nodiscard]] std::uint64_t TotalDequeued() const noexcept {
    return read_pos_.load(std::memory_order_relaxed);
  }

 private:
  // 只读共享数据独占一行，避免与频繁改动的索引挤在一起。
  TETHERKITNEXT_CACHE_ALIGNED const std::size_t capacity_;
  const std::size_t mask_;

  TETHERKITNEXT_CACHE_ALIGNED std::atomic<std::uint64_t> write_pos_{0};
  TETHERKITNEXT_CACHE_ALIGNED std::uint64_t cached_read_pos_{0};
  TETHERKITNEXT_CACHE_ALIGNED std::atomic<std::uint64_t> read_pos_{0};
  TETHERKITNEXT_CACHE_ALIGNED std::uint64_t cached_write_pos_{0};
};

/// 定长元素的 SPSC 无锁有界队列。
///
/// 元素类型要求可平凡复制（POD）：数据路径上不接受可能分配内存或抛异常的类型。
template <typename T>
  requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class SpscRing {
 public:
  /// `capacity` 会被向上取整到 2 的幂。
  explicit SpscRing(std::size_t capacity)
      : cursor_(RoundUpCapacity(capacity)),
        slots_(std::make_unique<T[]>(RoundUpCapacity(capacity))) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;
  ~SpscRing() = default;

  [[nodiscard]] std::size_t Capacity() const noexcept { return cursor_.Capacity(); }

  /// 入队。队列满返回 false（调用方决定丢弃还是重试）。
  [[nodiscard]] bool TryPush(const T& value) noexcept {
    std::size_t index = 0;
    if (!cursor_.TryAcquireWrite(index)) {
      return false;
    }
    slots_[index] = value;
    cursor_.PublishWrite();
    return true;
  }

  /// 出队。队列空返回 false。
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
