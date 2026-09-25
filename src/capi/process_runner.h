// 执行系统工具（ipconfig / route）并收集输出。
//
// ★ 为什么不用 system() / popen() ★
//   那两个都会经过 /bin/sh。网卡名、IP 地址这些参数虽然我们都校验过，但只要
//   经过 shell，防线就依赖「校验有没有漏」这一条；用 posix_spawn 直接传 argv
//   数组，shell 元字符从原理上就没有解释它们的地方。
//
// ★ 为什么用外部工具而不是自己发 ioctl ★
//   `ipconfig set` 走的是 IPConfiguration 的正规注册路径，由它建立的服务会被
//   IPMonitor 采纳（DNS 生效、默认路由装上）；而自己 SIOCAIFADDR 配出来的地址
//   configd 完全不认。这一条是 docs/GUI-SPIKE.md 第 3.2 / 6.3 节实测确认的，
//   不能为了「少一次 fork」倒退回裸 ioctl。
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::capi {

struct ProcessResult {
  int exit_code = 0;
  /// 子进程的 stdout 与 stderr 合并后的内容。
  ///
  /// 合并是刻意的：这些工具的报错有的走 stdout 有的走 stderr，分开收集只会让
  /// 「失败了但我们没拿到原因」这种情况更常见。
  std::string output;

  [[nodiscard]] bool Succeeded() const noexcept { return exit_code == 0; }
};

/// 执行一个可执行文件并等它结束。
///
/// @param executable 绝对路径。必须是绝对路径 —— 依赖 PATH 会让行为随调用者的
///                   环境变化，而 helper 是 launchd 拉起的，PATH 与终端里不同。
/// @param arguments  不含 argv[0]，函数内部会补上。
///
/// 返回错误仅表示「没能把进程跑起来或没等到它结束」；进程跑起来但返回非零，是
/// 成功返回一个 exit_code != 0 的 ProcessResult —— 调用方要自己判断。
[[nodiscard]] Result<ProcessResult> RunTool(std::string_view executable,
                                            std::initializer_list<std::string_view> arguments);

/// 供动态构造参数列表的重载。
[[nodiscard]] Result<ProcessResult> RunTool(std::string_view executable,
                                            const std::vector<std::string>& arguments);

}  // namespace tetherkitnext::capi
