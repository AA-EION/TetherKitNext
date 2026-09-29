// Registration entry point of the RNDIS codec microbenchmarks.
#pragma once

namespace tetherkitnext::bench {

class Runner;

/// Registers the RNDIS codec microbenchmarks with the runner.
void RegisterRndisBenchmarks(Runner& runner);

}  // namespace tetherkitnext::bench
