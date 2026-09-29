// TetherKitNext version information.
//
// The version number is injected into version.cc by CMake at configure time; the header only exposes accessor functions,
// avoiding a full-project rebuild every time the version number changes.
#pragma once

#include <cstdint>
#include <string_view>

namespace tetherkitnext {

/// The three numeric components of a semantic version.
struct Version {
  std::uint16_t major;
  std::uint16_t minor;
  std::uint16_t patch;
};

/// Returns the version number burned in at compile time.
Version GetVersion() noexcept;

/// Returns a readable version string like "TetherKitNext 0.1.0 (C++23, macOS 13.3+)".
std::string_view GetVersionString() noexcept;

/// Returns a description of the build configuration, like
/// "RelWithDebInfo, AppleClang 21.0.0, C++23, LTO=off, sanitizers=none".
///
/// Performance benchmark reports must include it -- an ns/op number detached from the build configuration is meaningless.
std::string_view GetBuildDescription() noexcept;

}  // namespace tetherkitnext
