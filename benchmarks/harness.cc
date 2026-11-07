#include "harness.h"

#include <sys/sysctl.h>
#include <sys/types.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "tetherkitnext/common/cache.h"
#include "tetherkitnext/version.h"

namespace tetherkitnext::bench {
namespace {

/// Reads a string-type sysctl; returns "unknown" on failure.
std::string SysctlString(const char* name) {
  std::size_t size = 0;
  if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return "unknown";
  }
  std::string value(size, '\0');
  if (::sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return "unknown";
  }
  // The length sysctl returns includes the terminator; strip it.
  while (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value;
}

/// Reads an integer-type sysctl; returns -1 on failure.
std::int64_t SysctlInt(const char* name) {
  std::int64_t value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
    return -1;
  }
  return value;
}

/// Chooses a suitable unit by order of magnitude, avoiding a long string of zeros in the table.
std::string FormatOpsPerSecond(double ops) {
  std::array<char, 32> buffer{};
  if (ops >= 1e9) {
    std::snprintf(buffer.data(), buffer.size(), "%.2f G/s", ops / 1e9);
  } else if (ops >= 1e6) {
    std::snprintf(buffer.data(), buffer.size(), "%.2f M/s", ops / 1e6);
  } else if (ops >= 1e3) {
    std::snprintf(buffer.data(), buffer.size(), "%.2f K/s", ops / 1e3);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%.0f /s", ops);
  }
  return buffer.data();
}

std::string FormatThroughput(const Result& result) {
  if (result.bytes_per_op == 0) {
    return "—";
  }
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.2f Gbps", result.Gigabitsps());
  return buffer.data();
}

/// Adds an exclamation mark when dispersion exceeds 30%, hinting to the reader that this measurement was disturbed by the environment.
std::string FormatSpread(const Result& result) {
  std::array<char, 32> buffer{};
  const double spread = result.Spread();
  std::snprintf(buffer.data(), buffer.size(), "%.1f%%%s", spread * 100.0,
                spread > 0.30 ? " ⚠" : "");
  return buffer.data();
}

}  // namespace

void PrintEnvironment() {
  const std::int64_t logical = SysctlInt("hw.logicalcpu");
  const std::int64_t performance = SysctlInt("hw.perflevel0.logicalcpu");
  const std::int64_t efficiency = SysctlInt("hw.perflevel1.logicalcpu");
  const std::int64_t cacheline = SysctlInt("hw.cachelinesize");

  std::printf("## Environment\n\n");
  std::printf("| Item | Value |\n|---|---|\n");
  std::printf("| CPU | %s |\n", SysctlString("machdep.cpu.brand_string").c_str());
  std::printf("| Logical cores | %lld (%lld performance / %lld efficiency) |\n", logical, performance, efficiency);
  std::printf("| Cache line | %lld bytes (kCacheLineSize = %zu) |\n", cacheline, kCacheLineSize);
  std::printf("| OS | macOS %s (%s) |\n", SysctlString("kern.osproductversion").c_str(),
              SysctlString("kern.osversion").c_str());
  const std::string build{GetBuildDescription()};
  std::printf("| Build | %s |\n", build.c_str());
  std::printf("\n");
}

void PrintMarkdownReport(const std::vector<Result>& results, std::string_view title) {
  std::printf("## %.*s\n\n", static_cast<int>(title.size()), title.data());
  std::printf("| Group | Benchmark | Median ns/op | Best ns/op | Rate | Throughput | Spread |\n");
  std::printf("|---|---|---:|---:|---:|---:|---:|\n");

  std::string current_group;
  for (const Result& result : results) {
    // Within the same group, show the group name only in the first row, which makes the table easier to read.
    const std::string group_cell = result.group == current_group ? "" : result.group;
    if (!group_cell.empty()) {
      current_group = result.group;
    }
    std::printf("| %s | %s | %.2f | %.2f | %s | %s | %s |\n", group_cell.c_str(),
                result.name.c_str(), result.median_nanos_per_op, result.best_nanos_per_op,
                FormatOpsPerSecond(result.OpsPerSecond()).c_str(), FormatThroughput(result).c_str(),
                FormatSpread(result).c_str());
  }
  std::printf("\n");
  std::printf("> Spread = (worst round − best round) / median. Above 30%% it is flagged ⚠: something ran in\n");
  std::printf("> the background or the run landed on an efficiency core, so treat the absolute numbers as rough.\n\n");
}

}  // namespace tetherkitnext::bench
