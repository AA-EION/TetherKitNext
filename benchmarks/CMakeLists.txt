# benchmarks -- performance benchmarks
#
# google/benchmark is not pulled in: this project only needs three kinds of metrics -- "nanoseconds per frame / pps / Gbps" --
# and the bundled lightweight harness (see benchmarks/harness.h) is sufficient, and it can directly emit a Markdown
# summary table to be written into docs/BENCHMARKS.md, saving a parsing step.

add_executable(
  tetherkitnext_bench
  bench_main.cc
  bench_common.cc
  bench_net.cc
  bench_rndis.cc
  harness.cc)

# tk_core links all the other modules PUBLICly (the benchmarks use rndis codecs and the net loopback).
target_link_libraries(tetherkitnext_bench PRIVATE tetherkitnext_base tk_version tk_core)
target_include_directories(tetherkitnext_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
