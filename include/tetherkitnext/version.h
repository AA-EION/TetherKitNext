// TetherKitNext 版本信息。
//
// 版本号由 CMake 在配置阶段注入到 version.cc，头文件只暴露访问函数，
// 避免每次改版本号都触发全项目重编译。
#pragma once

#include <cstdint>
#include <string_view>

namespace tetherkitnext {

/// 语义化版本号的三段数字。
struct Version {
  std::uint16_t major;
  std::uint16_t minor;
  std::uint16_t patch;
};

/// 返回编译期烧入的版本号。
Version GetVersion() noexcept;

/// 返回形如 "TetherKitNext 0.1.0 (C++23, macOS 13.3+)" 的可读版本串。
std::string_view GetVersionString() noexcept;

/// 返回构建配置描述，形如
/// "RelWithDebInfo, AppleClang 21.0.0, C++23, LTO=off, sanitizers=none"。
///
/// 性能基准报告必须带上它 —— 脱离构建配置的 ns/op 数字没有意义。
std::string_view GetBuildDescription() noexcept;

}  // namespace tetherkitnext
