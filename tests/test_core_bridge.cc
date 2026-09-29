// End-to-end tests of the bridge layer.
//
// MockDataChannel (USB side) + LoopbackLink (NIC side) are used to run the complete data path,
// covering real multi-threaded movement, batching, backpressure, pausing and graceful shutdown.
//
// These test cases are only fully meaningful when run under ThreadSanitizer:
//   cmake -B build-tsan -DTETHERKITNEXT_ENABLE_TSAN=ON && ctest --test-dir build-tsan -R core
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <doctest.h>

#include "language_guard.h"

#include "mock_data_channel.h"
#include "tetherkitnext/core/bridge.h"
#include "tetherkitnext/net/loopback_link.h"

using namespace tetherkitnext;        // NOLINT(google-build-using-namespace)
using namespace tetherkitnext::core;  // NOLINT(google-build-using-namespace)
using tetherkitnext::net::LoopbackConfig;
using tetherkitnext::net::LoopbackLink;
using tetherkitnext::testing::MockDataChannel;

namespace {

/// Builds an Ethernet frame whose content is self-verifying.
std::vector<std::byte> MakeFrame(std::uint32_t length, std::uint8_t tag) {
  std::vector<std::byte> frame(length);
  for (int i = 0; i < 6; ++i) {
    frame[static_cast<std::size_t>(i)] = std::byte{0xFF};  // broadcast destination MAC
  }
  frame[6] = std::byte{0x02};
  frame[11] = std::byte{tag};
  frame[12] = std::byte{0x08};
  frame[13] = std::byte{0x00};
  for (std::uint32_t i = 14; i < length; ++i) {
    frame[i] = std::byte{static_cast<unsigned char>((tag + i) & 0xFFU)};
  }
  return frame;
}

std::vector<std::vector<std::byte>> MakeFrames(std::uint32_t count, std::uint32_t length) {
  std::vector<std::vector<std::byte>> frames;
  frames.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    frames.push_back(MakeFrame(length, static_cast<std::uint8_t>(i & 0xFFU)));
  }
  return frames;
}

/// Polls and waits for a condition to hold, for at most `timeout`.
///
/// The data path is asynchronous, and one cannot "wait for it to finish" by sleeping a fixed duration -- that would
/// fail intermittently on slow machines. Here we wait for the condition rather than for time.
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

/// The bridge configuration used by tests: small queues and small batches, making boundaries easier to trigger.
[[nodiscard]] BridgeConfig TestConfig() {
  BridgeConfig config;
  config.rx_ring_frames = 256;
  config.max_frame_bytes = 1518;
  config.rx_write_batch = 16;
  config.rx_spin_before_park = 4;  // park quickly in tests
  config.tx_submit_batch = 32;
  return config;
}

}  // namespace

TEST_SUITE("core.bridge") {

TEST_CASE("启动与停止：线程正常起停，停机时通道被关闭") {
  MockDataChannel channel;
  LoopbackLink link;
  Bridge bridge(channel, link, TestConfig());

  CHECK_FALSE(bridge.Running());
  REQUIRE(bridge.Start().has_value());
  CHECK(bridge.Running());
  CHECK(channel.Receiving());

  // A repeated Start should be rejected.
  CHECK_FALSE(bridge.Start().has_value());

  bridge.Stop();
  CHECK_FALSE(bridge.Running());
  CHECK(channel.ShutdownCalled());

  // Stop is idempotent.
  bridge.Stop();
}

TEST_CASE("RX 方向：设备发来的帧被搬到链路上，内容完整") {
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  constexpr std::uint32_t kCount = 100;
  const auto frames = MakeFrames(kCount, 1514);
  CHECK(channel.InjectFromDevice(frames) == kCount);

  REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));

  const auto received = link.DrainSent();
  REQUIRE(received.size() == kCount);
  // FIFO order and content must both be preserved.
  for (std::size_t i = 0; i < kCount; ++i) {
    CHECK(received[i] == frames[i]);
  }

  const BridgeStats stats = bridge.Snapshot();
  CHECK(stats.rx.frames == kCount);
  CHECK(stats.rx.bytes == static_cast<std::uint64_t>(kCount) * 1514);

  bridge.Stop();
}

TEST_CASE("TX 方向：链路收到的帧被提交给设备，内容完整") {
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  constexpr std::uint32_t kCount = 100;
  const auto frames = MakeFrames(kCount, 512);
  for (const std::vector<std::byte>& frame : frames) {
    REQUIRE(link.PushInbound(frame));
  }

  REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= kCount; }));

  const auto delivered = channel.DrainSentToDevice();
  REQUIRE(delivered.size() == kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    CHECK(delivered[i] == frames[i]);
  }

  const BridgeStats stats = bridge.Snapshot();
  CHECK(stats.tx.frames == kCount);

  bridge.Stop();
}

TEST_CASE("双向同时跑，互不干扰") {
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 8192, .sent_capacity = 8192,
                                   .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  constexpr std::uint32_t kCount = 500;
  const auto tx_frames = MakeFrames(kCount, 700);

  // TX side: the host sends to the device.
  std::thread pusher([&] {
    for (const std::vector<std::byte>& frame : tx_frames) {
      while (!link.PushInbound(frame)) {
        std::this_thread::yield();
      }
    }
  });

  // RX side: the device sends to the host. Inject in batches, retrying when the queue is full.
  std::uint32_t injected = 0;
  while (injected < kCount) {
    const auto chunk = MakeFrames(std::min<std::uint32_t>(32, kCount - injected), 1000);
    const std::uint32_t accepted = channel.InjectFromDevice(chunk);
    injected += accepted;
    if (accepted == 0) {
      std::this_thread::yield();
    }
  }
  pusher.join();

  REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= kCount; }));
  REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));

  const BridgeStats stats = bridge.Snapshot();
  CHECK(stats.rx.frames >= kCount);
  CHECK(stats.tx.frames >= kCount);

  bridge.Stop();
}

TEST_CASE("RX 队列满时丢弃并计数，不阻塞生产者") {
  MockDataChannel channel;
  // The link's write capacity is set very small, so the RX injection thread cannot write out and the queue fills up quickly.
  LoopbackLink link(LoopbackConfig{.sent_capacity = 8});
  auto config = TestConfig();
  config.rx_ring_frames = 16;
  Bridge bridge(channel, link, config);
  REQUIRE(bridge.Start().has_value());

  // Inject far more frames than the queue capacity.
  std::uint32_t total_accepted = 0;
  for (int round = 0; round < 20; ++round) {
    total_accepted += channel.InjectFromDevice(MakeFrames(64, 1514));
  }

  // Key assertion: InjectFromDevice never blocks, and rejected frames are counted as drops.
  const BridgeStats stats = bridge.Snapshot();
  CHECK(total_accepted < 20 * 64);  // rejection really occurred
  CHECK(stats.rx.TotalDropped() > 0);

  bridge.Stop();
}

TEST_CASE("TX 背压：传输池占满时等待而不丢弃，容量恢复后全部送达") {
  // This is a regression test for the root cause of dropped frames on real hardware: the old implementation immediately dropped the batch's remaining frames when SendFrames returned 0
  // (bridge.cc once had a wrong argument that "waiting turns into invisible kernel drops"),
  // dropping 30%+ of TX traffic at the door of the transfer pool under high load. The correct behavior is to wait for a slot to free up and then retry
  // -- bursts are absorbed by the upstream kernel BPF buffer.
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());

  // Make the mock channel accept not a single frame -- simulating the transfer pool being completely full.
  channel.SetAcceptLimit(0);
  REQUIRE(bridge.Start().has_value());

  constexpr std::uint32_t kFrameCount = 200;
  for (const std::vector<std::byte>& frame : MakeFrames(kFrameCount, 512)) {
    REQUIRE(link.PushInbound(frame));
  }

  // The bridge layer should go into waiting: backpressure events rise and WaitForSendCapacity gets called,
  // but not a single frame is dropped, and not a single frame is sent out.
  REQUIRE(WaitFor([&] { return bridge.Snapshot().tx_backpressure_events > 0; }));
  REQUIRE(WaitFor([&] { return channel.WaitCalls() > 0; }));
  CHECK(bridge.Snapshot().tx.TotalDropped() == 0);
  CHECK(channel.SentFrameCount() == 0);

  // Capacity is restored (equivalent to an in-flight transfer completing and the slot being returned): the frames that had been stuck at the door must
  // be delivered **without losing a single frame**, and in the same order.
  // The wait condition must cover both the mock count and the bridge-layer count: the mock increments inside SendFrames first,
  // and the bridge layer only does AddBatch after returning, with no happens-before between the two -- waiting only on the mock count and
  // then asserting the bridge-layer count would intermittently read an old value one slice behind on slow machines.
  channel.SetAcceptLimit(0xFFFF'FFFFU);
  REQUIRE(WaitFor([&] {
    return channel.SentFrameCount() >= kFrameCount &&
           bridge.Snapshot().tx.frames >= kFrameCount;
  }));

  const BridgeStats stats = bridge.Snapshot();
  CHECK(stats.tx.TotalDropped() == 0);
  CHECK(stats.tx.frames == kFrameCount);
  CHECK(channel.SentFrameCount() == kFrameCount);

  const auto delivered = channel.DrainSentToDevice();
  REQUIRE(delivered.size() == kFrameCount);
  const auto expected = MakeFrames(kFrameCount, 512);
  for (std::size_t i = 0; i < delivered.size(); ++i) {
    CHECK(delivered[i] == expected[i]);
  }

  bridge.Stop();
}

TEST_CASE("TX 背压：部分接纳时剩余帧重试送达，不丢不乱序") {
  // The pool is not full but cannot fit a whole slice at once (SendFrames partially accepts): the remaining frames must be retried in place.
  // At most 7 frames each time and a slice of 32 frames, forcing out the path of "multiple partial accepts completing one slice".
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());

  channel.SetAcceptLimit(7);
  REQUIRE(bridge.Start().has_value());

  constexpr std::uint32_t kFrameCount = 100;
  for (const std::vector<std::byte>& frame : MakeFrames(kFrameCount, 512)) {
    REQUIRE(link.PushInbound(frame));
  }

  REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= kFrameCount; }));
  CHECK(bridge.Snapshot().tx.TotalDropped() == 0);

  const auto delivered = channel.DrainSentToDevice();
  REQUIRE(delivered.size() == kFrameCount);
  const auto expected = MakeFrames(kFrameCount, 512);
  for (std::size_t i = 0; i < delivered.size(); ++i) {
    CHECK(delivered[i] == expected[i]);
  }

  bridge.Stop();
}

TEST_CASE("TX 背压：等待期间 Stop() 能及时返回") {
  // The backpressure wait introduces a new blocking point, and shutdown must not be stuck by it: the TX thread must re-check the shutdown flag on every wakeup (at most about
  // 50 ms).
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());

  channel.SetAcceptLimit(0);
  REQUIRE(bridge.Start().has_value());

  for (const std::vector<std::byte>& frame : MakeFrames(200, 512)) {
    REQUIRE(link.PushInbound(frame));
  }
  REQUIRE(WaitFor([&] { return bridge.Snapshot().tx_backpressure_events > 0; }));

  const auto stop_begin = std::chrono::steady_clock::now();
  bridge.Stop();
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_begin;

  CHECK_FALSE(bridge.Running());
  // The upper bound is "wait timeout + BPF read timeout + slack". The real constraint is on the order of tens of milliseconds,
  // and it is relaxed to 2 seconds here only to avoid flaky failures on slow CI.
  CHECK(stop_elapsed < std::chrono::seconds(2));
  // Frames not sent out at shutdown are counted as drops -- the books must balance, and frames cannot vanish out of thin air.
  CHECK(bridge.Snapshot().tx.TotalDropped() > 0);
}

TEST_CASE("TX 背压：等待期间落下暂停，剩余帧被丢弃且线程存活") {
  // Pausing (RNDIS soft reset) takes priority over waiting: the device will discard outstanding packets anyway, and hoarding them is meaningless.
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());

  channel.SetAcceptLimit(0);
  REQUIRE(bridge.Start().has_value());

  for (const std::vector<std::byte>& frame : MakeFrames(100, 512)) {
    REQUIRE(link.PushInbound(frame));
  }
  REQUIRE(WaitFor([&] { return bridge.Snapshot().tx_backpressure_events > 0; }));
  CHECK(bridge.Snapshot().tx.TotalDropped() == 0);

  bridge.SetPaused(true);
  REQUIRE(WaitFor([&] { return bridge.Snapshot().tx.TotalDropped() > 0; }));
  CHECK(channel.SentFrameCount() == 0);

  // After resuming, new frames are delivered as usual -- the thread is not stuck in a wait.
  bridge.SetPaused(false);
  channel.SetAcceptLimit(0xFFFF'FFFFU);
  for (const std::vector<std::byte>& frame : MakeFrames(20, 512)) {
    REQUIRE(link.PushInbound(frame));
  }
  REQUIRE(WaitFor([&] { return channel.SentFrameCount() >= 20; }));

  bridge.Stop();
}

TEST_CASE("暂停期间不搬运，恢复后 RX 队列里的帧继续送出") {
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  bridge.SetPaused(true);
  CHECK(bridge.Paused());

  constexpr std::uint32_t kCount = 32;
  CHECK(channel.InjectFromDevice(MakeFrames(kCount, 256)) == kCount);

  // No frames should be written to the link during the pause. Give the injection thread a bit of time to prove it really did not move.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(link.TotalSentFrames() == 0);
  // The frames are still in the queue and have not been dropped.
  CHECK(bridge.Snapshot().rx_queue_depth == kCount);

  // After resuming they should all be sent out -- pausing does not lose frames.
  bridge.SetPaused(false);
  REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));
  CHECK(bridge.Snapshot().rx.TotalDropped() == 0);

  bridge.Stop();
}

TEST_CASE("SetPaused 在任何生命周期阶段都不会挂死") {
  // SetPaused(true) waits for the RX injection thread to confirm it has stopped. This wait has two hang risks,
  // both far more severe than the frame-leak problem it is meant to fix (what hangs is the control path: link state changes, device soft reset):
  //   * when the thread is not running at all, nobody comes to set the acknowledgement bit;
  //   * a leftover acknowledgement bit makes the wait logic itself go wrong.
  // Here all three phases are walked through, and a hang at any point makes this test case time out rather than pass silently.
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
  Bridge bridge(channel, link, TestConfig());

  // Phase one: before Start().
  bridge.SetPaused(true);
  CHECK(bridge.Paused());
  bridge.SetPaused(false);

  REQUIRE(bridge.Start().has_value());

  // Phase two: rapid toggling while running. The acknowledgement bit is cleared at the start of every loop iteration, so that a later SetPaused(true)
  // does not read the previous round's leftover value and return early -- returning early means the guarantee is void.
  for (int i = 0; i < 50; ++i) {
    bridge.SetPaused(true);
    bridge.SetPaused(false);
  }

  // After toggling, the real pause guarantee is still obtainable.
  bridge.SetPaused(true);
  constexpr std::uint32_t kCount = 16;
  CHECK(channel.InjectFromDevice(MakeFrames(kCount, 256)) == kCount);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(link.TotalSentFrames() == 0);

  bridge.Stop();

  // Phase three: after Stop().
  bridge.SetPaused(true);
  CHECK(bridge.Paused());
}

TEST_CASE("暂停期间 TX 方向的帧被丢弃并计数") {
  // Unlike RX: the TX side must drop while paused, because during an RNDIS reset the device discards all outstanding
  // data packets, and hoarding them only wastes memory.
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 512, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());
  bridge.SetPaused(true);

  for (const std::vector<std::byte>& frame : MakeFrames(50, 256)) {
    REQUIRE(link.PushInbound(frame));
  }

  REQUIRE(WaitFor([&] { return bridge.Snapshot().tx.TotalDropped() > 0; }));
  CHECK(channel.SentFrameCount() == 0);

  bridge.Stop();
}

TEST_CASE("链路写失败不会让线程退出，恢复后继续工作") {
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
  link.FailWritesAfter(1);  // fail starting from the 2nd write
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  for (int round = 0; round < 5; ++round) {
    // Here we do not care how many frames were enqueued; it is enough to generate traffic that "writes the link repeatedly".
    (void)channel.InjectFromDevice(MakeFrames(16, 256));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Key: the bridge layer is still running, and did not crash or exit because of write failures.
  CHECK(bridge.Running());
  CHECK(bridge.Snapshot().rx.io_errors > 0);

  bridge.Stop();
}

TEST_CASE("USB 提交失败被记为 I/O 错误，桥接层继续运行") {
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 512, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());
  channel.SetSendShouldFail(true);
  REQUIRE(bridge.Start().has_value());

  for (const std::vector<std::byte>& frame : MakeFrames(50, 256)) {
    REQUIRE(link.PushInbound(frame));
  }

  REQUIRE(WaitFor([&] { return bridge.Snapshot().tx.io_errors > 0; }));
  CHECK(bridge.Running());

  bridge.Stop();
}

TEST_CASE("统计行渲染出速率与丢包") {
  // The statistics line is user-facing text; the assertions below are in Chinese, so pin the language first.
  const tetherkitnext::testing::ScopedLanguage guard{tetherkitnext::Language::kChinese};
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  const BridgeStats before = bridge.Snapshot();
  constexpr std::uint32_t kCount = 50;
  CHECK(channel.InjectFromDevice(MakeFrames(kCount, 1514)) == kCount);
  REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));
  const BridgeStats after = bridge.Snapshot();

  const std::string line = FormatStatsLine(before, after, 1.0);
  CHECK(line.find("RX") != std::string::npos);
  CHECK(line.find("TX") != std::string::npos);
  CHECK(line.find("pps") != std::string::npos);
  CHECK(line.find("Mbps") != std::string::npos);
  CHECK(line.find("队列深度") != std::string::npos);

  bridge.Stop();
}

TEST_CASE("停机时不丢已在队列里的统计，且能在有流量时安全停下") {
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 8192, .sent_capacity = 8192,
                                   .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  // Shutting down while traffic flows -- this is the scenario that most easily exposes teardown-order problems.
  std::atomic<bool> keep_going{true};
  std::thread producer([&] {
    while (keep_going.load(std::memory_order_acquire)) {
      (void)channel.InjectFromDevice(MakeFrames(8, 512));
      for (const std::vector<std::byte>& frame : MakeFrames(8, 512)) {
        (void)link.PushInbound(frame);
      }
      std::this_thread::yield();
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  bridge.Stop();  // shut down while there is traffic
  keep_going.store(false, std::memory_order_release);
  producer.join();

  CHECK_FALSE(bridge.Running());
  CHECK(channel.ShutdownCalled());
}

}  // TEST_SUITE("core.bridge")
