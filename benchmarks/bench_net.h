// Registration entry point of the feth / BPF link-layer benchmarks.
//
// Unlike the other benchmarks, this group **needs root** (it must create a feth pair and open /dev/bpf*).
// Without root nothing is registered, and the reason is stated through skip_reason -- the report must make visible
// that "this group did not run", rather than letting it vanish silently.
#pragma once

#include <string>

namespace tetherkitnext::bench {

class Runner;

/// Builds the feth + BPF fixture and registers the link-layer benchmarks.
///
/// @param skip_reason The skip reason written when false is returned.
/// @return Whether registration succeeded. The fixture's lifetime is held by this module's static object,
///         and it lives until process exit.
bool RegisterNetBenchmarks(Runner& runner, std::string& skip_reason);

/// Tears down the fixture (closes the BPF descriptor, destroys the feth pair).
///
/// **Must be called explicitly before main returns.** Letting the fixture live into the static destruction phase crashes:
/// FethDevice's destructor writes logs, and by then the logging module's static mutex may already have been
/// destroyed first, so it throws `std::system_error: mutex lock failed` and the process exits with 134 --
/// the benchmarks had clearly all finished, yet the exit code is a failure.
void ShutdownNetBenchmarks() noexcept;

}  // namespace tetherkitnext::bench
