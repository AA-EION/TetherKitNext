// 轻量性能基准 harness。
//
// 为什么不用 google/benchmark：
//   本项目只关心三类指标 —— 每次操作的纳秒数、每秒帧数（pps）、每秒比特数
//   （Gbps）。google/benchmark 的自动迭代次数推断、统计模型与 JSON 输出都是
//   我们不需要的，而它会引入 FetchContent 的联网依赖并显著拖慢构建。
//   这个 harness 只有 200 行，却能直接输出可以贴进 docs/BENCHMARKS.md 的
//   Markdown 表格，省掉一层解析。
//
// 度量方法（每项基准）：
//   1. 预热若干轮，让分支预测器、缓存与 CPU 频率进入稳态；
//   2. 正式跑 N 轮，每轮内部循环固定次数；
//   3. 取各轮**中位数**而非均值 —— macOS 上后台进程与 big.LITTLE 调度会造成
//      偶发的长尾，中位数对此更稳健；同时报告最好值与相对离散度，便于判断
//      这次测量是否可信。
#pragma once

#include <algorithm>
#include <ranges>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tetherkitnext/common/time.h"

namespace tetherkitnext::bench {

/// 阻止编译器把被测代码优化掉。
///
/// 内联汇编把值声明为「被读取且内存可能被修改」，编译器因此不能消除产生它的
/// 计算。这是 google/benchmark 的 DoNotOptimize 的同等实现。
template <typename T>
inline void DoNotOptimize(T const& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

/// 让编译器认为所有内存都可能被读写，用于分隔两段不该被合并的代码。
inline void ClobberMemory() {
  asm volatile("" : : : "memory");
}

/// 单项基准的度量结果。
struct Result {
  std::string name;
  std::string group;

  std::uint64_t operations = 0;  ///< 每轮的操作数（帧数 / 入队次数 / ...）。
  std::uint64_t bytes_per_op = 0;  ///< 每次操作搬运的字节数；0 表示不适用。

  double median_nanos_per_op = 0.0;
  double best_nanos_per_op = 0.0;
  double worst_nanos_per_op = 0.0;

  /// 相对离散度 =（最差 - 最好）/ 中位数。超过 0.3 说明测量环境噪声大。
  [[nodiscard]] double Spread() const {
    return median_nanos_per_op > 0.0 ? (worst_nanos_per_op - best_nanos_per_op) / median_nanos_per_op
                                     : 0.0;
  }

  /// 每秒操作数（中位数）。
  [[nodiscard]] double OpsPerSecond() const {
    return median_nanos_per_op > 0.0 ? 1e9 / median_nanos_per_op : 0.0;
  }

  /// 吞吐（Gbps）。仅当 bytes_per_op > 0 时有意义。
  [[nodiscard]] double Gigabitsps() const {
    return bytes_per_op == 0 ? 0.0
                             : OpsPerSecond() * static_cast<double>(bytes_per_op) * 8.0 / 1e9;
  }
};

/// 一次基准运行的配置。
struct Config {
  std::uint32_t warmup_rounds = 3;   ///< 预热轮数，结果不计入。
  std::uint32_t measure_rounds = 9;  ///< 正式轮数，取中位数（奇数便于取中）。
  std::uint64_t ops_per_round = 0;   ///< 每轮的操作数；由各基准自行决定。
  std::uint64_t bytes_per_op = 0;    ///< 每次操作的字节数，用于换算 Gbps。
};

/// 基准注册表与执行器。
class Runner {
 public:
  /// 被测函数签名：给定本轮要执行的操作数，返回实际执行的操作数。
  ///
  /// 返回值而非固定使用入参，是为了让「跑到队列满就停」这类基准也能诚实报告。
  using Body = std::function<std::uint64_t(std::uint64_t)>;

  /// 注册一项基准。`group` 用于在汇总表里分组。
  void Add(std::string group, std::string name, Config config, Body body) {
    entries_.push_back(Entry{std::move(group), std::move(name), config, std::move(body)});
  }

  /// 依次执行全部基准，边跑边打印进度。
  [[nodiscard]] std::vector<Result> RunAll() {
    std::vector<Result> results;
    results.reserve(entries_.size());
    for (const Entry& entry : entries_) {
      std::fprintf(stderr, "  正在测量 %s / %s ...\n", entry.group.c_str(), entry.name.c_str());
      results.push_back(RunOne(entry));
    }
    return results;
  }

 private:
  struct Entry {
    std::string group;
    std::string name;
    Config config;
    Body body;
  };

  static Result RunOne(const Entry& entry) {
    const Config& config = entry.config;

    // 预热：不记录，只为让缓存与频率进入稳态。
    for (std::uint32_t i = 0; i < config.warmup_rounds; ++i) {
      DoNotOptimize(entry.body(config.ops_per_round));
    }

    std::vector<double> nanos_per_op;
    nanos_per_op.reserve(config.measure_rounds);
    for (std::uint32_t i = 0; i < config.measure_rounds; ++i) {
      ClobberMemory();
      const Nanos start = MonotonicNanos();
      const std::uint64_t executed = entry.body(config.ops_per_round);
      const Nanos elapsed = MonotonicNanos() - start;
      ClobberMemory();

      if (executed == 0) {
        continue;
      }
      nanos_per_op.push_back(static_cast<double>(elapsed) / static_cast<double>(executed));
    }

    Result result;
    result.group = entry.group;
    result.name = entry.name;
    result.operations = config.ops_per_round;
    result.bytes_per_op = config.bytes_per_op;
    if (nanos_per_op.empty()) {
      return result;
    }

    std::ranges::sort(nanos_per_op);
    result.best_nanos_per_op = nanos_per_op.front();
    result.worst_nanos_per_op = nanos_per_op.back();
    result.median_nanos_per_op = nanos_per_op[nanos_per_op.size() / 2];
    return result;
  }

  std::vector<Entry> entries_;
};

/// 把结果集渲染成 Markdown 表格，可直接粘进 docs/BENCHMARKS.md。
void PrintMarkdownReport(const std::vector<Result>& results, std::string_view title);

/// 打印本机环境信息（CPU、核数、缓存行、编译配置），基准结果必须带上下文才有意义。
void PrintEnvironment();

}  // namespace tetherkitnext::bench
