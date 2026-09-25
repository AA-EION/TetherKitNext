# benchmarks —— 性能基准
#
# 不引入 google/benchmark：本项目只需要「每帧纳秒数 / pps / Gbps」这三类指标，
# 自带的轻量 harness（见 benchmarks/harness.h）足够，且能直接输出 Markdown
# 汇总表写进 docs/BENCHMARKS.md，省掉一层解析。

add_executable(
  tetherkitnext_bench
  bench_main.cc
  bench_common.cc
  bench_net.cc
  bench_rndis.cc
  harness.cc)

# tk_core PUBLIC 链接了其余全部模块（基准要用到 rndis 编解码与 net loopback）。
target_link_libraries(tetherkitnext_bench PRIVATE tetherkitnext_base tk_version tk_core)
target_include_directories(tetherkitnext_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
