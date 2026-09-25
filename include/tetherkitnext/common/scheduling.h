// 线程调度策略。
//
// 为什么需要它：Apple Silicon 是 big.LITTLE 架构（本机 10 核 = 4 性能核 +
// 6 效率核）。默认 QoS 下，数据路径线程可能被调度到效率核上，吞吐会明显下降，
// 而且抖动变大。macOS **不提供**把线程绑定到特定物理核的接口
// （没有 sched_setaffinity），能做的是通过 QoS class 表达意图，由内核决定：
//
//   QOS_CLASS_USER_INTERACTIVE —— 最高优先级，内核优先安排到性能核。
//   QOS_CLASS_USER_INITIATED   —— 次之。
//   QOS_CLASS_DEFAULT          —— 默认。
//   QOS_CLASS_UTILITY / BACKGROUND —— 倾向效率核。
//
// 我们给数据路径线程用 USER_INTERACTIVE，给统计/日志之类的辅助线程用 UTILITY。
//
// 关于实时线程（THREAD_TIME_CONSTRAINT_POLICY）：
//   Core Audio 那套 time-constraint 策略能拿到更强的时延保证，但它要求线程
//   在声明的时间预算内主动让出，超时会被降级惩罚。我们的 BPF read() 是阻塞
//   调用，时长不可预测，不满足 time-constraint 的使用前提，因此不采用。
#pragma once

#include <pthread.h>

#include <cstdint>
#include <string_view>

namespace tetherkitnext {

/// 线程用途，决定 QoS 等级。
enum class ThreadRole : std::uint8_t {
  kDataPath,  ///< USB 事件循环、BPF 读写 —— 吞吐与时延关键，争取性能核。
  kControl,   ///< RNDIS 控制通道、保活 —— 低频但需要及时响应。
  kAuxiliary,  ///< 统计报告、日志刷盘 —— 可以让路。
};

/// 给当前线程设置名字与 QoS。应在线程函数最开头调用一次。
///
/// 名字同时写入 thread_local（供日志前缀使用）与内核（供 lldb / Instruments）。
void ConfigureCurrentThread(std::string_view name, ThreadRole role) noexcept;

/// 把 ThreadRole 映射到 qos_class_t 的数值。单独暴露以便测试与日志打印。
[[nodiscard]] unsigned int QosClassFor(ThreadRole role) noexcept;

}  // namespace tetherkitnext
