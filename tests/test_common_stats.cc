// stats.h 与 time.h 的单元测试。
#include <chrono>
#include <thread>

#include <doctest.h>

#include "tetherkitnext/common/stats.h"
#include "tetherkitnext/common/time.h"

using tetherkitnext::DirectionCounters;
using tetherkitnext::kNanosPerMilli;
using tetherkitnext::MonotonicNanos;
using tetherkitnext::PathCounters;
using tetherkitnext::PeriodicTimer;
using tetherkitnext::RateSampler;
using tetherkitnext::Snapshot;
using tetherkitnext::Stopwatch;

TEST_SUITE("common.stats") {

TEST_CASE("计数器累加与快照") {
  DirectionCounters counters;
  counters.AddFrame(1514);
  counters.AddFrame(64);
  counters.AddDroppedFull(3);
  counters.AddDroppedOversize();
  counters.AddDroppedMalformed(2);
  counters.AddIoError();

  const auto snapshot = Snapshot(counters);
  CHECK(snapshot.frames == 2);
  CHECK(snapshot.bytes == 1578);
  CHECK(snapshot.dropped_full == 3);
  CHECK(snapshot.dropped_oversize == 1);
  CHECK(snapshot.dropped_malformed == 2);
  CHECK(snapshot.io_errors == 1);
  CHECK(snapshot.TotalDropped() == 6);
}

TEST_CASE("批次计数用于计算平均批大小") {
  DirectionCounters counters;
  counters.AddBatch(10, 15140);
  counters.AddBatch(6, 9084);

  const auto snapshot = Snapshot(counters);
  CHECK(snapshot.frames == 16);
  CHECK(snapshot.bytes == 24224);
  CHECK(snapshot.batches == 2);
}

TEST_CASE("快照相减得到窗口增量") {
  DirectionCounters counters;
  counters.AddFrame(100);
  const auto first = Snapshot(counters);
  counters.AddFrame(200);
  counters.AddDroppedFull();
  const auto second = Snapshot(counters);

  const auto delta = second - first;
  CHECK(delta.frames == 1);
  CHECK(delta.bytes == 200);
  CHECK(delta.dropped_full == 1);
}

TEST_CASE("速率采样把字节数换算成 Mbps") {
  PathCounters counters;
  RateSampler sampler;

  // 造 1 秒内 12.5 MB 的流量 = 100 Mbps。
  counters.rx.AddBatch(8000, 12'500'000);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto report = sampler.Sample(counters);

  CHECK(report.seconds > 0.0);
  // 只跑了约 20ms，所以速率会被放大约 50 倍；这里只校验换算关系是否自洽。
  const double expected_mbps = 12'500'000.0 * 8.0 / 1'000'000.0 / report.seconds;
  CHECK(report.rx_mbps == doctest::Approx(expected_mbps).epsilon(0.01));
  CHECK(report.rx_avg_batch == doctest::Approx(8000.0));
  CHECK(report.tx_pps == doctest::Approx(0.0));
}

TEST_CASE("秒表读数单调不减") {
  const Stopwatch watch;
  const auto first = watch.Elapsed();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto second = watch.Elapsed();
  CHECK(second >= first);
  CHECK(watch.ElapsedMillis() >= 1.0);
}

TEST_CASE("周期定时器的默认构造仍然是「构造即开始计时」") {
  PeriodicTimer timer(50 * kNanosPerMilli);
  // 刚构造完不该到期。
  CHECK_FALSE(timer.Expired());
  CHECK(timer.RemainingNanos(MonotonicNanos()) > 0);
  CHECK(timer.Period() == 50 * kNanosPerMilli);
}

TEST_CASE("周期定时器按累加期限推进，不随调度延迟漂移") {
  const tetherkitnext::Nanos now = MonotonicNanos();
  // 必须把 `now` 作为计时起点显式传进去。若让构造函数自己读时钟，它读到的时刻
  // 会比这里的 `now` 略晚，于是「now + period」反而还没到期，断言就会随机失败。
  PeriodicTimer timer(10 * kNanosPerMilli, now);

  CHECK_FALSE(timer.Expired(now));
  CHECK(timer.RemainingNanos(now) > 0);

  // 恰好到期。
  CHECK(timer.Expired(now + 10 * kNanosPerMilli));
  // 刚触发过，立刻再问应为未到期。
  CHECK_FALSE(timer.Expired(now + 10 * kNanosPerMilli));
  // 再过一个周期又到期。
  CHECK(timer.Expired(now + 20 * kNanosPerMilli));
}

TEST_CASE("周期定时器落后超过一个周期时不补发一串触发") {
  const tetherkitnext::Nanos now = MonotonicNanos();
  PeriodicTimer timer(10 * kNanosPerMilli, now);

  // 线程被挂起 1 秒后才醒来：应该只触发一次，然后对齐到当前时刻之后。
  const tetherkitnext::Nanos late = now + 1000 * kNanosPerMilli;
  CHECK(timer.Expired(late));
  CHECK_FALSE(timer.Expired(late));
  CHECK_FALSE(timer.Expired(late + 9 * kNanosPerMilli));
  CHECK(timer.Expired(late + 10 * kNanosPerMilli));
}

}  // TEST_SUITE("common.stats")
