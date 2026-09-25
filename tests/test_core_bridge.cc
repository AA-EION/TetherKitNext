// 桥接层的端到端测试。
//
// 用 MockDataChannel（USB 侧）+ LoopbackLink（网卡侧）把完整的数据路径跑起来，
// 覆盖真实的多线程搬运、批处理、背压、暂停与优雅停机。
//
// 这些用例在 ThreadSanitizer 下跑才有完整意义：
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

/// 造一帧内容可自校验的以太帧。
std::vector<std::byte> MakeFrame(std::uint32_t length, std::uint8_t tag) {
  std::vector<std::byte> frame(length);
  for (int i = 0; i < 6; ++i) {
    frame[static_cast<std::size_t>(i)] = std::byte{0xFF};  // 广播目的 MAC
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

/// 轮询等待某个条件成立，最多等 `timeout`。
///
/// 数据路径是异步的，不能用 sleep 一个固定时长来「等它跑完」—— 那样在慢机器上
/// 会偶发失败。这里等条件而不是等时间。
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

/// 测试用的桥接配置：小队列、小批次，让边界更容易被触发。
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

  // 重复 Start 应被拒绝。
  CHECK_FALSE(bridge.Start().has_value());

  bridge.Stop();
  CHECK_FALSE(bridge.Running());
  CHECK(channel.ShutdownCalled());

  // Stop 是幂等的。
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
  // FIFO 顺序与内容都必须保持。
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

  // TX 侧：主机往设备发。
  std::thread pusher([&] {
    for (const std::vector<std::byte>& frame : tx_frames) {
      while (!link.PushInbound(frame)) {
        std::this_thread::yield();
      }
    }
  });

  // RX 侧：设备往主机发。分批注入，队列满时重试。
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
  // 链路的写出容量设很小，让 RX 注入线程写不出去，队列很快堆满。
  LoopbackLink link(LoopbackConfig{.sent_capacity = 8});
  auto config = TestConfig();
  config.rx_ring_frames = 16;
  Bridge bridge(channel, link, config);
  REQUIRE(bridge.Start().has_value());

  // 注入远超队列容量的帧。
  std::uint32_t total_accepted = 0;
  for (int round = 0; round < 20; ++round) {
    total_accepted += channel.InjectFromDevice(MakeFrames(64, 1514));
  }

  // 关键断言：InjectFromDevice 从不阻塞，而且被拒的帧被计入丢弃。
  const BridgeStats stats = bridge.Snapshot();
  CHECK(total_accepted < 20 * 64);  // 确实发生了拒收
  CHECK(stats.rx.TotalDropped() > 0);

  bridge.Stop();
}

TEST_CASE("TX 背压：传输池占满时等待而不丢弃，容量恢复后全部送达") {
  // 这是真机丢帧根因的回归测试：旧实现在 SendFrames 返回 0 时立即丢弃批次
  // 剩余帧（bridge.cc 曾有一条「等待会变成不可见的内核丢包」的错误论证），
  // 高负载下把 30%+ 的 TX 流量丢在了传输池门口。正确行为是等槽位空出来再重
  // 试 —— 突发由上游的内核 BPF 缓冲吸收。
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.inbound_capacity = 4096, .max_frames_per_batch = 64});
  Bridge bridge(channel, link, TestConfig());

  // 让 mock 通道一帧都不收 —— 模拟传输池完全占满。
  channel.SetAcceptLimit(0);
  REQUIRE(bridge.Start().has_value());

  constexpr std::uint32_t kFrameCount = 200;
  for (const std::vector<std::byte>& frame : MakeFrames(kFrameCount, 512)) {
    REQUIRE(link.PushInbound(frame));
  }

  // 桥接层应转入等待：背压事件在涨、WaitForSendCapacity 被调用，
  // 但一帧都没有丢、也一帧都没有发出。
  REQUIRE(WaitFor([&] { return bridge.Snapshot().tx_backpressure_events > 0; }));
  REQUIRE(WaitFor([&] { return channel.WaitCalls() > 0; }));
  CHECK(bridge.Snapshot().tx.TotalDropped() == 0);
  CHECK(channel.SentFrameCount() == 0);

  // 容量恢复（相当于在飞传输完成、槽位归还）：先前顶在门口的帧必须
  // **一帧不少**地送达，且顺序不变。
  // 等待条件必须同时覆盖 mock 计数与桥接层计数：mock 在 SendFrames 内部先递增、
  // 桥接层在返回后才 AddBatch，两者之间没有 happens-before —— 只等 mock 计数就
  // 断言桥接层计数，会在慢机器上偶发读到差一个分片的旧值。
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
  // 池没满但一次装不下整个分片（SendFrames 部分接纳）：剩余帧必须原地重试。
  // 每次最多吃 7 帧、分片 32 帧，逼出「多次部分接纳拼完一个分片」的路径。
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
  // 背压等待引入了新的阻塞点，停机不能被它卡住：TX 线程每次醒来（至多约
  // 50 ms）都要重查停机标志。
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
  // 上界取「等待超时 + BPF 读超时 + 富余」。真实约束是几十毫秒量级，
  // 这里放宽到 2 秒只为不在慢 CI 上闪断。
  CHECK(stop_elapsed < std::chrono::seconds(2));
  // 停机时未送出的帧计入丢弃 —— 账要能对上，不能凭空消失。
  CHECK(bridge.Snapshot().tx.TotalDropped() > 0);
}

TEST_CASE("TX 背压：等待期间落下暂停，剩余帧被丢弃且线程存活") {
  // 暂停（RNDIS 软复位）优先于等待：设备反正会丢弃未完成的包，攒着没有意义。
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

  // 恢复后新的帧照常送达 —— 线程没有卡死在等待里。
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

  // 暂停期间不该有帧被写到链路上。给注入线程一点时间证明它确实没动。
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(link.TotalSentFrames() == 0);
  // 帧还在队列里，没被丢弃。
  CHECK(bridge.Snapshot().rx_queue_depth == kCount);

  // 恢复后应全部送出 —— 暂停不丢帧。
  bridge.SetPaused(false);
  REQUIRE(WaitFor([&] { return link.TotalSentFrames() >= kCount; }));
  CHECK(bridge.Snapshot().rx.TotalDropped() == 0);

  bridge.Stop();
}

TEST_CASE("SetPaused 在任何生命周期阶段都不会挂死") {
  // SetPaused(true) 会等 RX 注入线程确认它停住了。这个等待有两种挂死风险，
  // 都比它要修的漏帧问题严重得多（挂的是控制路径：链路状态变化、设备软复位）：
  //   * 线程压根没在跑时没人来置确认位；
  //   * 确认位残留导致等待逻辑本身出错。
  // 这里把三个阶段都走一遍，任何一处挂死都会让本用例超时而不是静默通过。
  MockDataChannel channel;
  LoopbackLink link(LoopbackConfig{.sent_capacity = 4096});
  Bridge bridge(channel, link, TestConfig());

  // 阶段一：Start() 之前。
  bridge.SetPaused(true);
  CHECK(bridge.Paused());
  bridge.SetPaused(false);

  REQUIRE(bridge.Start().has_value());

  // 阶段二：运行中快速 toggle。确认位每轮循环开头清零，后一次 SetPaused(true)
  // 才不会读到上一轮的残留值就提前返回 —— 提前返回等于保证失效。
  for (int i = 0; i < 50; ++i) {
    bridge.SetPaused(true);
    bridge.SetPaused(false);
  }

  // toggle 之后仍然拿得到真实的暂停保证。
  bridge.SetPaused(true);
  constexpr std::uint32_t kCount = 16;
  CHECK(channel.InjectFromDevice(MakeFrames(kCount, 256)) == kCount);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(link.TotalSentFrames() == 0);

  bridge.Stop();

  // 阶段三：Stop() 之后。
  bridge.SetPaused(true);
  CHECK(bridge.Paused());
}

TEST_CASE("暂停期间 TX 方向的帧被丢弃并计数") {
  // 与 RX 不同：TX 侧暂停时必须丢，因为 RNDIS 复位期间设备会丢弃所有未完成的
  // 数据包，攒着只是浪费内存。
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
  link.FailWritesAfter(1);  // 第 2 次写起失败
  Bridge bridge(channel, link, TestConfig());
  REQUIRE(bridge.Start().has_value());

  for (int round = 0; round < 5; ++round) {
    // 这里不关心入队了几帧，只要制造出「反复写链路」的流量即可。
    (void)channel.InjectFromDevice(MakeFrames(16, 256));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // 关键：桥接层仍在运行，没有因为写失败而崩掉或退出。
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
  // 统计行是面向用户的文案，下面按中文断言，先把语言钉死。
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

  // 一边有流量一边停机 —— 这是最容易暴露拆除顺序问题的场景。
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
  bridge.Stop();  // 有流量时停机
  keep_going.store(false, std::memory_order_release);
  producer.join();

  CHECK_FALSE(bridge.Running());
  CHECK(channel.ShutdownCalled());
}

}  // TEST_SUITE("core.bridge")
