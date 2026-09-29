// Unit tests of SpscRing / SpscCursor.
//
// The multi-threaded cases are only fully meaningful under a ThreadSanitizer build:
//   cmake -B build-tsan -DTETHERKITNEXT_ENABLE_TSAN=ON && ctest --test-dir build-tsan -R spsc
#include <atomic>
#include <cstdint>
#include <thread>

#include <doctest.h>

#include "tetherkitnext/common/cache.h"
#include "tetherkitnext/common/spsc_ring.h"

using tetherkitnext::kCacheLineSize;
using tetherkitnext::SpscRing;

namespace {

/// Used to verify that "the element content is really transferred in full" rather than just the index.
struct Payload {
  std::uint64_t sequence;
  std::uint32_t checksum;
};

constexpr std::uint32_t Checksum(std::uint64_t sequence) {
  return static_cast<std::uint32_t>((sequence * 2654435761ULL) >> 16U);
}

}  // namespace

TEST_SUITE("common.spsc_ring") {

TEST_CASE("容量向上取整到 2 的幂") {
  CHECK(SpscRing<std::uint32_t>(1).Capacity() == 1);
  CHECK(SpscRing<std::uint32_t>(3).Capacity() == 4);
  CHECK(SpscRing<std::uint32_t>(4).Capacity() == 4);
  CHECK(SpscRing<std::uint32_t>(1000).Capacity() == 1024);
}

TEST_CASE("空队列出队失败") {
  SpscRing<std::uint32_t> ring(4);
  std::uint32_t value = 0xFFFFFFFFU;
  CHECK_FALSE(ring.TryPop(value));
  CHECK(value == 0xFFFFFFFFU);  // the output parameter must not be modified on failure
  CHECK(ring.SizeSnapshot() == 0);
}

TEST_CASE("满队列入队失败且容量被完整利用") {
  // One of the design goals of the freely incrementing counters: no wasted slots. Capacity 4 should hold 4.
  SpscRing<std::uint32_t> ring(4);
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK(ring.TryPush(i));
  }
  CHECK(ring.SizeSnapshot() == 4);
  CHECK_FALSE(ring.TryPush(99));
}

TEST_CASE("FIFO 顺序与内容完整性") {
  SpscRing<Payload> ring(8);
  for (std::uint64_t i = 0; i < 8; ++i) {
    CHECK(ring.TryPush(Payload{.sequence = i, .checksum = Checksum(i)}));
  }
  for (std::uint64_t i = 0; i < 8; ++i) {
    Payload out{};
    REQUIRE(ring.TryPop(out));
    CHECK(out.sequence == i);
    CHECK(out.checksum == Checksum(i));
  }
  CHECK(ring.SizeSnapshot() == 0);
}

TEST_CASE("反复绕环不丢数据") {
  // Capacity 4, push 1000 elements, taking one immediately after each push, forcing the indices to wrap around repeatedly.
  SpscRing<std::uint64_t> ring(4);
  for (std::uint64_t i = 0; i < 1000; ++i) {
    REQUIRE(ring.TryPush(i));
    std::uint64_t out = 0;
    REQUIRE(ring.TryPop(out));
    CHECK(out == i);
  }
}

TEST_CASE("索引游标各字段落在不同缓存行") {
  // This is a performance correctness assertion: if false sharing regresses, this test case fails.
  SpscRing<std::uint32_t> ring(8);
  const auto& cursor = ring.Cursor();
  const auto base = reinterpret_cast<std::uintptr_t>(&cursor);
  // SpscCursor's four indices each have alignas(kCacheLineSize),
  // so the whole object must occupy at least 4 cache lines (plus the one for the read-only fields).
  CHECK(sizeof(tetherkitnext::SpscCursor) >= 4 * kCacheLineSize);
  CHECK(base % kCacheLineSize == 0);
}

TEST_CASE("单生产者单消费者并发搬运不丢不重不乱序") {
  constexpr std::uint64_t kTotal = 200'000;
  SpscRing<Payload> ring(1024);

  std::atomic<std::uint64_t> produced{0};
  std::atomic<bool> corrupted{false};

  std::thread producer([&] {
    std::uint64_t sequence = 0;
    while (sequence < kTotal) {
      if (ring.TryPush(Payload{.sequence = sequence, .checksum = Checksum(sequence)})) {
        ++sequence;
        produced.store(sequence, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  while (expected < kTotal) {
    Payload out{};
    if (ring.TryPop(out)) {
      // The order must be strictly increasing (FIFO), and the content must be self-consistent with the sequence number (no tearing).
      if (out.sequence != expected || out.checksum != Checksum(out.sequence)) {
        corrupted.store(true, std::memory_order_relaxed);
        break;
      }
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }

  producer.join();
  CHECK_FALSE(corrupted.load());
  CHECK(expected == kTotal);
  CHECK(ring.SizeSnapshot() == 0);
}

}  // TEST_SUITE("common.spsc_ring")
