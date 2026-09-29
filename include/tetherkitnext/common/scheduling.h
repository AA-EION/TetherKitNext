// Thread scheduling policy.
//
// Why it is needed: Apple Silicon is a big.LITTLE architecture (this machine's 10 cores = 4 performance cores +
// 6 efficiency cores). Under the default QoS, data-path threads may be scheduled onto efficiency cores, and throughput drops noticeably,
// with more jitter as well. macOS **does not provide** an interface to bind a thread to a specific physical core
// (there is no sched_setaffinity); what can be done is to express intent through the QoS class and let the kernel decide:
//
//   QOS_CLASS_USER_INTERACTIVE -- highest priority; the kernel prefers to place it on performance cores.
//   QOS_CLASS_USER_INITIATED   -- next.
//   QOS_CLASS_DEFAULT          -- default.
//   QOS_CLASS_UTILITY / BACKGROUND -- tend toward efficiency cores.
//
// We use USER_INTERACTIVE for data-path threads, and UTILITY for auxiliary threads such as statistics/logging.
//
// About real-time threads (THREAD_TIME_CONSTRAINT_POLICY):
//   The Core Audio style time-constraint policy can get stronger latency guarantees, but it requires the thread
//   to yield voluntarily within the declared time budget, and overrunning is penalized with demotion. Our BPF read() is a blocking
//   call of unpredictable duration, which does not satisfy the prerequisites of time-constraint, so it is not adopted.
#pragma once

#include <pthread.h>

#include <cstdint>
#include <string_view>

namespace tetherkitnext {

/// Thread purpose, which determines the QoS level.
enum class ThreadRole : std::uint8_t {
  kDataPath,  ///< USB event loop, BPF read/write -- critical for throughput and latency; aim for performance cores.
  kControl,   ///< RNDIS control channel, keepalive -- low frequency but needs a timely response.
  kAuxiliary,  ///< Statistics reports, log flushing -- may yield.
};

/// Sets the name and QoS of the current thread. Should be called once at the very start of the thread function.
///
/// The name is written both to thread_local (for the log prefix) and to the kernel (for lldb / Instruments).
void ConfigureCurrentThread(std::string_view name, ThreadRole role) noexcept;

/// Maps a ThreadRole to the numeric value of qos_class_t. Exposed separately for testing and log printing.
[[nodiscard]] unsigned int QosClassFor(ThreadRole role) noexcept;

}  // namespace tetherkitnext
