// Performance benchmark entry point.
//
// The output is a complete Markdown document (on standard output), which can be redirected directly into
// docs/BENCHMARKS.md:
//
//   ./build-rel/bin/tetherkitnext_bench > docs/BENCHMARKS.md
//
// Progress information goes to standard error and does not pollute the report.
#include <cstdio>
#include <string>
#include <vector>

#include "bench_common.h"
#include "bench_net.h"
#include "bench_rndis.h"
#include "harness.h"
#include "tetherkitnext/common/scheduling.h"
#include "tetherkitnext/version.h"

int main() {
  // The benchmark thread itself also runs with the data path's QoS, otherwise it might be scheduled onto efficiency cores,
  // and the measured numbers would be worse than at actual runtime.
  tetherkitnext::ConfigureCurrentThread("bench", tetherkitnext::ThreadRole::kDataPath);

  const std::string version{tetherkitnext::GetVersionString()};
  std::printf("# TetherKitNext 性能基准\n\n");
  std::printf("由 `tetherkitnext_bench` 自动生成 —— %s\n\n", version.c_str());
  std::printf("> 重新生成：`./build-rel/bin/tetherkitnext_bench > docs/BENCHMARKS.md`\n");
  std::printf("> 必须使用 **未启用消毒器的 Release 构建**，否则数字无参考价值。\n\n");

  tetherkitnext::bench::PrintEnvironment();

  std::fprintf(stderr, "开始测量……\n");

  {
    tetherkitnext::bench::Runner runner;
    tetherkitnext::bench::RegisterCommonBenchmarks(runner);
    tetherkitnext::bench::PrintMarkdownReport(runner.RunAll(), "基础设施层（tk_common）");
  }
  {
    tetherkitnext::bench::Runner runner;
    tetherkitnext::bench::RegisterRndisBenchmarks(runner);
    tetherkitnext::bench::PrintMarkdownReport(runner.RunAll(), "RNDIS 编解码（tk_rndis）");
  }
  {
    // The link layer needs root. When skipped, a trace must also be left in the report -- otherwise the reader cannot tell whether this report
    // means "tested and fine" or "not tested at all".
    tetherkitnext::bench::Runner runner;
    std::string skip_reason;
    if (tetherkitnext::bench::RegisterNetBenchmarks(runner, skip_reason)) {
      tetherkitnext::bench::PrintMarkdownReport(runner.RunAll(), "feth / BPF 链路层（tk_net，需 root）");
      // It must be torn down here and cannot be left to static destruction -- see bench_net.h for the reason.
      tetherkitnext::bench::ShutdownNetBenchmarks();
    } else {
      std::printf("## feth / BPF 链路层（tk_net，需 root）\n\n");
      std::printf("**本次未测量** —— %s。\n\n", skip_reason.c_str());
      std::printf("补测方法：`sudo ./build/bin/tetherkitnext_bench > docs/BENCHMARKS.md`\n\n");
      std::fprintf(stderr, "  跳过链路层基准：%s\n", skip_reason.c_str());
    }
  }

  std::fprintf(stderr, "测量完成。\n");
  return 0;
}
