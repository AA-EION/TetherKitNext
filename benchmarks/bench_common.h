// Registration entry point of the infrastructure-layer microbenchmarks.
#pragma once

namespace tetherkitnext::bench {

class Runner;

/// Registers all of tk_common's microbenchmarks with the runner.
void RegisterCommonBenchmarks(Runner& runner);

}  // namespace tetherkitnext::bench
