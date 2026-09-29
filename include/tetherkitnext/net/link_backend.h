// Link-layer backend abstraction.
//
// Why an abstraction is needed: BPF needs root and a real feth interface, and cannot run on a development machine.
// After abstracting "send and receive raw Ethernet frames" into a narrow interface, end-to-end tests and throughput benchmarks can run with an in-memory
// loopback backend in any environment.
//
// Performance trade-off: the interface uses **virtual functions**, but virtual calls are **not at per-frame granularity** --
// ReadFrames / WriteFrames are both batch interfaces; one virtual call handles dozens to hundreds of frames,
// and the overhead amortized per frame is far below 1 ns, entirely negligible.
// (One virtual call per frame would be unacceptable; that is a design deliberately avoided.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/common/frame_ring.h"

namespace tetherkitnext::net {

/// A batch of frames to send.
using FrameBatch = std::span<const FrameView>;

/// Result of one batch read.
struct ReadBatch {
  /// Frames decoded in this batch; views point into the backend's internal buffer and are valid until the next ReadFrames.
  std::span<const FrameView> frames;
  /// Kernel-side cumulative drop count (BPF's bs_drop). Used for backpressure warnings.
  std::uint64_t kernel_drops = 0;
};

/// Result of one batch write.
struct WriteResult {
  std::uint32_t frames_written = 0;
  std::uint64_t bytes_written = 0;
  /// Number of frames skipped for reasons such as a single frame being too long.
  std::uint32_t frames_skipped = 0;
};

/// A backend that sends and receives raw Ethernet frames.
class LinkBackend {
 public:
  LinkBackend() = default;
  LinkBackend(const LinkBackend&) = delete;
  LinkBackend& operator=(const LinkBackend&) = delete;
  LinkBackend(LinkBackend&&) = delete;
  LinkBackend& operator=(LinkBackend&&) = delete;
  virtual ~LinkBackend() = default;

  /// Blocking read of a batch of frames.
  ///
  /// Returning an empty batch is legal (for example when interrupted by a signal); the caller should continue looping.
  /// Only a real error returns Error.
  [[nodiscard]] virtual Result<ReadBatch> ReadFrames() = 0;

  /// Writes out a batch of frames. Best effort to write them all; oversized frames are skipped and counted.
  [[nodiscard]] virtual Result<WriteResult> WriteFrames(FrameBatch frames) = 0;

  /// Per-frame length limit (including the 14-byte Ethernet header).
  [[nodiscard]] virtual std::uint32_t MaxFrameBytes() const noexcept = 0;

  /// Whether writing multiple frames per system call is supported. Used for logs and benchmark reports.
  [[nodiscard]] virtual bool SupportsBatchWrite() const noexcept = 0;

  /// Wakes the thread blocked in ReadFrames, used for graceful shutdown.
  ///
  /// Must be safe to call from **other threads**.
  virtual void Interrupt() noexcept = 0;
};

}  // namespace tetherkitnext::net
