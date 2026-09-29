// BPF link backend: reads and writes raw Ethernet frames directly on the driver-side interface of feth.
//
// * Every ioctl ordering and parameter choice in this file has a source-level reason; read all the comments before changing anything *
//
// Configuration sequence (the order has hard constraints, from xnu bsd/net/bpf.c):
//   1. open("/dev/bpf%d")  -- macOS **has no** /dev/bpf cloning node; the numbers must be iterated.
//   2. BIOCSBLEN           -- **must come before BIOCSETIF** (bpf.c has
//                             `if (d->bd_bif != 0) return EINVAL`).
//   3. BIOCSHDRCMPLT = 1   -- **must come before BIOCSBATCHWRITE**, and it decides whether frames
//                             pass through as-is (see below).
//   4. BIOCSETIF           -- bind to the interface; only afterwards can the DLT be queried.
//   5. BIOCGDLT            -- verify it is DLT_EN10MB.
//   6. BIOCIMMEDIATE = 1   -- determines the latency characteristics of reads (see below).
//   7. BIOCSSEESENT = 0    -- see only the input direction (see below).
//   8. BIOCSBATCHWRITE = 1 -- optional optimization, feature probe.
//   9. BIOCSNOTSTAMP = 1   -- optional optimization, feature probe.
//
// The three most critical parameter choices:
//
//   * **BIOCSHDRCMPLT = 1 is mandatory.** With hdrcmplt=0, bpfwrite strips the first
//     14 bytes of user data into a sockaddr and lets ether_frameout rebuild the frame header -- the source MAC would be
//     rewritten by the driver, and the frames forwarded out would no longer be the original frames sent by the device. Only hdrcmplt=1 takes
//     DLIL_OUTPUT_FLAGS_RAW, passing frames through as-is.
//
//   * **BIOCSSEESENT = 0 happens to filter out the frames we wrote ourselves.** When attached to the driver-side interface:
//     frames the host sends from the system side are the **input** direction here; frames we write in are the
//     **output** direction here. SEESENT=0 -> bd_direction = BPF_D_IN, keeping only input,
//     which is exactly what we want to forward to USB. If not set, we would read back the frames we just wrote, forming a loop.
//
//   * **BIOCIMMEDIATE = 1 + a dedicated thread doing blocking read() is the optimal model; do not use kqueue.**
//     Under immediate, bpf_wakeup fires on every arriving packet, and after read() wakes up it delivers all packets accumulated in the meantime
//     as one batch -- low latency at low rates, automatically aggregating into large batches at high rates (behavior similar to Linux's NAPI),
//     costing only one system call per batch. The kqueue approach has exactly the same readiness criterion, but needs an extra kevent().
//     And if immediate is not enabled while wanting to avoid "returns only once the buffer fills", the only option is to rely on
//     BIOCSRTIMEOUT -- but its actual resolution is pinned by the 10 ms clock tick
//     (the kernel stores tvtohz(tv)-1, hz=100 on this machine), and the latency is unacceptable.
//
// Two read details that must be remembered:
//
//   * **The read() buffer length must exactly equal bd_bufsize**, otherwise bpfread returns at the start with
//     `if (uio_resid(uio) != d->bd_bufsize) return EINVAL`. And BIOCSBLEN, when the limit is exceeded, **does not report an error**,
//     silently clamps to the cap and writes the actual value back into the parameter, so the written-back value must be used.
//
//   * **Records must be iterated with bh_hdrlen, not sizeof(struct bpf_hdr).**
//     Under LP64, sizeof is 20 (the 18 bytes of content are padded), while the kernel writes
//     a bh_hdrlen of 18 for DLT_EN10MB. Using 20 would immediately misalign.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/net/link_backend.h"

namespace tetherkitnext::net {

/// Configuration parameters of the BPF backend.
struct BpfConfig {
  /// Kernel capture buffer size.
  ///
  /// The upper limit is the sysctl debug.bpf_bufsize_cap (32 MiB on macOS 13+); exceeding it is silently
  /// truncated. The kernel allocates **2 copies** of this size for every descriptor (store + free double buffering),
  /// so the actual wired memory usage is twice this value.
  ///
  /// Derivation of the 4 MiB default: at full 1 Gbps speed, 1514-byte frames are about 82k pps; with a single copyout
  /// window estimated at a worst-case 10 ms, about 1.2 MB of buffering is needed; 4 MiB leaves a 3x margin. Anything larger just
  /// wastes wired memory, since BPF has only two buffers, and enlarging each one does not improve the packet-loss window.
  std::uint32_t kernel_buffer_bytes = 4U * 1024 * 1024;

  /// Per-frame length limit (including the Ethernet header). Should equal interface MTU + Ethernet header, and must not exceed MTU + 18.
  std::uint32_t max_frame_bytes = 1518;

  /// The maximum number of frames one ReadFrames call can decode.
  ///
  /// This is the capacity of the FrameView array. A 4 MiB buffer can hold at most about 4Mi/32 ≈ 131k minimum-size
  /// frames (64-byte frame + 18-byte header + alignment), but in practice they will not all be minimum-size; 8192 covers
  /// the vast majority of batches, and the excess is left to be parsed after the next read.
  std::size_t max_frames_per_batch = 8192;

  /// Whether to try enabling batch writes (BIOCSBATCHWRITE, macOS 14+).
  bool try_batch_write = true;

  /// Whether to try turning off capture timestamps (BIOCSNOTSTAMP), saving one microtime() per frame.
  bool try_disable_timestamp = true;

  /// Read timeout. Determines "how long after Interrupt() we can stop when there is no traffic at all".
  ///
  /// Note that the kernel's actual resolution is 10 ms (it stores tvtohz(tv) - 1 ticks, and on this machine
  /// kern.clockrate hz=100), and passing {0,0} becomes **blocking forever**. 200 ms lets
  /// shutdown be timely enough without producing needless wakeups.
  std::uint32_t read_timeout_millis = 200;
};

/// A BPF descriptor attached to a specified interface.
class BpfLink final : public LinkBackend {
 public:
  /// Opens and configures a BPF descriptor, binding it to `interface_name`.
  ///
  /// Requires root: the permissions of /dev/bpf* are 0600 root:wheel, and macOS **has no**
  /// access_bpf group like FreeBSD's to go through.
  [[nodiscard]] static Result<std::unique_ptr<BpfLink>> Open(std::string_view interface_name,
                                                            const BpfConfig& config);

  // Copy and move are already deleted in the base class LinkBackend; they are restated explicitly here to satisfy static analysis.
  BpfLink(const BpfLink&) = delete;
  BpfLink& operator=(const BpfLink&) = delete;
  BpfLink(BpfLink&&) = delete;
  BpfLink& operator=(BpfLink&&) = delete;
  ~BpfLink() override;

  [[nodiscard]] Result<ReadBatch> ReadFrames() override;
  [[nodiscard]] Result<WriteResult> WriteFrames(FrameBatch frames) override;

  [[nodiscard]] std::uint32_t MaxFrameBytes() const noexcept override { return max_frame_bytes_; }

  [[nodiscard]] bool SupportsBatchWrite() const noexcept override { return batch_write_enabled_; }

  void Interrupt() noexcept override;

  /// The buffer size actually in effect in the kernel (may have been clamped to the upper limit).
  [[nodiscard]] std::uint32_t KernelBufferBytes() const noexcept { return kernel_buffer_bytes_; }

  /// Path of the opened device node, for logging.
  [[nodiscard]] std::string_view DevicePath() const noexcept { return device_path_; }

  /// Kernel-side statistics: cumulative number of packets received / dropped.
  struct KernelStats {
    std::uint64_t received = 0;
    std::uint64_t dropped = 0;
  };

  [[nodiscard]] Result<KernelStats> QueryKernelStats() const;

 private:
  BpfLink() = default;

  /// Per-frame write(); the fallback path when batch writes are unavailable.
  [[nodiscard]] Result<WriteResult> WriteFramesIndividually(FrameBatch frames);

  /// Multiple frames per write() (BIOCSBATCHWRITE).
  [[nodiscard]] Result<WriteResult> WriteFramesBatched(FrameBatch frames);

  int fd_ = -1;
  std::string device_path_;
  std::string interface_name_;

  std::uint32_t kernel_buffer_bytes_ = 0;
  std::uint32_t max_frame_bytes_ = 0;
  bool batch_write_enabled_ = false;
  bool timestamp_disabled_ = false;

  /// Read buffer. Its length must exactly equal kernel_buffer_bytes_ (a hard requirement of bpfread).
  std::vector<std::byte> read_buffer_;
  /// Frame views decoded in this batch, pointing into read_buffer_.
  std::vector<FrameView> read_frames_;
  /// The kernel's cumulative drop count (bs_drop) from the most recent successful read.
  ///
  /// When BIOCGSTATS occasionally fails, **keep the previous value** rather than reporting 0: bs_drop is a cumulative counter,
  /// and consumers use it for "current - previous" differencing; reporting 0 would make the difference under/overflow on unsigned numbers,
  /// printing an astronomical "kernel drops" figure. Accessed only by the read thread, so no atomic is needed.
  std::uint64_t last_kernel_drops_ = 0;
  /// Assembly buffer for batch writes.
  std::vector<std::byte> write_buffer_;

  /// Stop flag. After Interrupt() sets it, ReadFrames returns an empty batch within one read-timeout period.
  ///
  /// Choice of wakeup mechanism: BPF's read() blocks, and there are three ways to interrupt it --
  ///   (a) send a signal so BPF_SLEEP (with PCATCH) returns EINTR;
  ///   (b) close the fd (has a use-after-close race; not acceptable);
  ///   (c) set a moderate BIOCSRTIMEOUT so read() wakes up periodically to check this flag.
  /// Choose (c): no signal handler needs to be installed in the process, and there is no race. BIOCIMMEDIATE=1 guarantees
  /// it still returns immediately when there are packets; the read timeout only affects "how quickly we respond to shutdown when there is no traffic at all".
  std::atomic<bool> interrupted_{false};
};

}  // namespace tetherkitnext::net
