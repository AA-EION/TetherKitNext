// feth / BPF 链路层基准的注册入口。
//
// 与其余基准不同，这一组**需要 root**（要建 feth 网卡对、要开 /dev/bpf*）。
// 非 root 时不注册任何项，并通过 skip_reason 说明原因 —— 报告里要能看见
// 「这组没跑」，而不是让它静默消失。
#pragma once

#include <string>

namespace tetherkitnext::bench {

class Runner;

/// 建立 feth + BPF 夹具并注册链路层基准。
///
/// @param skip_reason 返回 false 时写入跳过原因。
/// @return 是否成功注册。夹具的生命周期由本模块的静态对象持有，
///         一直存活到进程退出。
bool RegisterNetBenchmarks(Runner& runner, std::string& skip_reason);

/// 拆掉夹具（关 BPF 描述符、销毁 feth 网卡对）。
///
/// **必须在 main 返回之前显式调用。** 放任夹具活到静态析构阶段会崩：
/// FethDevice 的析构函数会写日志，而那时日志模块的静态互斥量可能已经先一步
/// 被销毁，于是抛出 `std::system_error: mutex lock failed`，进程以 134 退出 ——
/// 基准明明全部跑完了，退出码却是失败。
void ShutdownNetBenchmarks() noexcept;

}  // namespace tetherkitnext::bench
