// Runs system tools (ipconfig / route) and collects their output.
//
// * Why not system() / popen() *
//   Both of them go through /bin/sh. Although we validate arguments such as NIC names and IP addresses, as long as
//   they go through a shell the line of defense depends on the single point "did validation miss anything"; with posix_spawn passing the argv
//   array directly, shell metacharacters in principle have no place that interprets them.
//
// * Why use external tools rather than issuing ioctls ourselves *
//   `ipconfig set` takes IPConfiguration's proper registration path, and services it establishes are adopted by
//   IPMonitor (DNS takes effect, the default route gets installed); while addresses configured by our own SIOCAIFADDR
//   are not recognized by configd at all. This was confirmed by measurement in sections 3.2 / 6.3 of docs/GUI-SPIKE.md,
//   and we must not regress to bare ioctls just to "save one fork".
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::capi {

struct ProcessResult {
  int exit_code = 0;
  /// The combined content of the child process's stdout and stderr.
  ///
  /// Combining is deliberate: some of these tools' errors go to stdout and some to stderr, and collecting them separately would only make
  /// the situation "it failed but we did not get the reason" more common.
  std::string output;

  [[nodiscard]] bool Succeeded() const noexcept { return exit_code == 0; }
};

/// Runs an executable and waits for it to finish.
///
/// @param executable An absolute path. It must be an absolute path -- depending on PATH would make behavior vary with the caller's
///                   environment, and the helper is launched by launchd, whose PATH differs from a terminal's.
/// @param arguments  Excluding argv[0], which the function adds internally.
///
/// Returning an error only means "could not get the process running or did not get to wait for it to finish"; if the process ran but returned non-zero, that is a
/// successful return of a ProcessResult with exit_code != 0 -- the caller must judge it themselves.
[[nodiscard]] Result<ProcessResult> RunTool(std::string_view executable,
                                            std::initializer_list<std::string_view> arguments);

/// Overload for dynamically constructed argument lists.
[[nodiscard]] Result<ProcessResult> RunTool(std::string_view executable,
                                            const std::vector<std::string>& arguments);

}  // namespace tetherkitnext::capi
