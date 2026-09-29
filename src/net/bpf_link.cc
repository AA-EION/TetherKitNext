#include "tetherkitnext/net/bpf_link.h"

#include <fcntl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/net/darwin_abi.h"
#include "tetherkitnext/net/feth_device.h"

namespace tetherkitnext::net {
namespace {

/// Upper limit for iterating /dev/bpf%d.
///
/// When the kernel opens the current last node it makes another on demand (in bpfopen,
/// `if (minor(dev) == nbpfilter - 1) bpf_make_dev_t(...)`), and the limit is controlled by the sysctl
/// debug.bpf_maxdevices (256 on this machine). 256 is used here to match.
constexpr int kMaxBpfDeviceIndex = 256;

/// Upper limit in bytes of one write() during batch writes.
///
/// The absolute upper limit the kernel allows is BPF_WRITE_MAX = 16 MiB, but there is no need for something that large:
/// 256 KiB already holds over a hundred full frames, and the fixed overhead of a single system call is long since amortized,
/// while a larger assembly buffer would only worsen the L2 cache hit rate.
constexpr std::size_t kMaxBatchWriteBytes = std::size_t{256} * 1024;

/// Issues one ioctl; on failure wraps errno and the call name into an Error.
[[nodiscard]] Status CallIoctl(int fd, unsigned long request, void* argument,
                               std::string_view what) {
  if (::ioctl(fd, request, argument) < 0) {
    return std::unexpected(Error::FromErrno(0, std::string{what}));
  }
  return Ok();
}

/// Tries an **optional** ioctl: failure is not an error; it only returns whether it succeeded.
[[nodiscard]] bool TryOptionalIoctl(int fd, unsigned long request, void* argument,
                                    std::string_view what) {
  if (::ioctl(fd, request, argument) < 0) {
    // ENOTTY / EINVAL means the current macOS version does not support this private ioctl, which is an expected case.
    TETHERKITNEXT_DEBUG_TR(Msg::kNetOptionalIoctlUnavailable, what, errno);
    return false;
  }
  return true;
}

}  // namespace

BpfLink::~BpfLink() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void BpfLink::Interrupt() noexcept {
  interrupted_.store(true, std::memory_order_release);
}

Result<std::unique_ptr<BpfLink>> BpfLink::Open(std::string_view interface_name,
                                              const BpfConfig& config) {
  if (interface_name.size() >= kInterfaceNameCapacity) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetInterfaceNameTooLong, interface_name)));
  }
  if (config.max_frame_bytes < kMinEthernetFrameBytes) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetFrameLimitBelowMinimum, config.max_frame_bytes, kMinEthernetFrameBytes)));
  }

  auto link = std::unique_ptr<BpfLink>(new BpfLink());
  link->interface_name_ = interface_name;
  link->max_frame_bytes_ = config.max_frame_bytes;

  // ---------------------------------------------------------------------------
  // 1. Open /dev/bpf%d
  //
  // macOS **has no** /dev/bpf cloning node (measured: ls /dev/bpf -> No such file),
  // so numbers must be tried one by one: EBUSY means that minor is already held by another process, try the next;
  // ENOENT means the limit of nodes the kernel has currently made has been reached.
  // ---------------------------------------------------------------------------
  int last_errno = 0;
  for (int index = 0; index < kMaxBpfDeviceIndex; ++index) {
    std::string path = std::format("/dev/bpf{}", index);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      link->fd_ = fd;
      link->device_path_ = std::move(path);
      break;
    }
    last_errno = errno;
    if (last_errno == EBUSY) {
      continue;  // taken by another capture program, move on to the next
    }
    if (last_errno == ENOENT) {
      break;  // reached the limit; there will be no nodes with larger numbers
    }
    if (last_errno == EACCES || last_errno == EPERM) {
      return std::unexpected(
          Error::FromErrno(last_errno, Tr(Msg::kNetBpfOpenDenied, path)));
    }
    // Other errno: keep trying the next number, and report an error uniformly at the end.
  }

  if (link->fd_ < 0) {
    return std::unexpected(
        Error::FromErrno(last_errno, Tr(Msg::kNetBpfNoDeviceAvailable)));
  }

  // ---------------------------------------------------------------------------
  // 2. BIOCSBLEN -- **must be before BIOCSETIF**
  //
  // In bpf.c: `if (d->bd_bif != 0 || (d->bd_flags & BPF_DETACHING)) return EINVAL`.
  // And when the limit is exceeded it **does not report an error**; it silently clamps to BPF_BUFSIZE_CAP and writes the actually effective value back
  // into the parameter through _IOWR -- so every later read() must use this written-back value as its length.
  // ---------------------------------------------------------------------------
  auto buffer_bytes = static_cast<unsigned int>(config.kernel_buffer_bytes);
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSBLEN, &buffer_bytes, "ioctl(BIOCSBLEN)"));
  link->kernel_buffer_bytes_ = buffer_bytes;
  if (buffer_bytes != config.kernel_buffer_bytes) {
    TETHERKITNEXT_INFO_TR(Msg::kNetBpfBufferClamped, config.kernel_buffer_bytes, buffer_bytes);
  }

  // ---------------------------------------------------------------------------
  // 3. BIOCSHDRCMPLT = 1 -- **mandatory**, and must be before BIOCSBATCHWRITE
  //
  // With hdrcmplt=0, bpfwrite strips the first 14 bytes to rebuild the frame header (the source MAC gets rewritten by the driver);
  // =1 takes DLIL_OUTPUT_FLAGS_RAW and passes through as-is. Batch writes also strictly require it to be 1.
  //
  // WARNING: And on feth the consequence is far more severe than "the frame header gets rewritten": **without this ioctl, write()
  // returns ENXIO directly** (if_fake's output path cannot handle the AF_UNSPEC rebuild branch).
  // See item 14 of section 7 in AGENTS.md for the controlled experiment. When troubleshooting ENXIO, do not suspect the interface name.
  // ---------------------------------------------------------------------------
  int header_complete = 1;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSHDRCMPLT, &header_complete, "ioctl(BIOCSHDRCMPLT)"));

  // ---------------------------------------------------------------------------
  // 4. BIOCSETIF -- bind to the interface
  // ---------------------------------------------------------------------------
  ::ifreq bind_request{};
  std::memcpy(bind_request.ifr_name, interface_name.data(), interface_name.size());
  if (const auto status =
          CallIoctl(link->fd_, BIOCSETIF, &bind_request, "ioctl(BIOCSETIF)");
      !status) {
    Error error = status.error();
    if (error.Code() == ENXIO) {
      return std::unexpected(std::move(error).WithContext(
          Tr(Msg::kNetBpfInterfaceMissing, interface_name)));
    }
    return std::unexpected(
        std::move(error).WithContext(Tr(Msg::kNetBpfBindFailed, interface_name)));
  }

  // ---------------------------------------------------------------------------
  // 5. BIOCGDLT -- verify the data link type
  //
  // feth's bpfattach uses DLT_EN10MB, so this is necessarily 1; it is verified to prevent
  // someone from mistakenly binding BPF to another interface type, in which case all frame format assumptions would be wrong.
  // ---------------------------------------------------------------------------
  unsigned int data_link_type = 0;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCGDLT, &data_link_type, "ioctl(BIOCGDLT)"));
  if (data_link_type != DLT_EN10MB) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetBpfWrongDataLinkType, interface_name, data_link_type, DLT_EN10MB)));
  }

  // ---------------------------------------------------------------------------
  // 6. BIOCIMMEDIATE = 1 -- immediate delivery
  //
  // If not enabled, read() returns only when the buffer **fills**; with a 4 MiB buffer the latency is catastrophically high.
  // Once enabled, it wakes on every arriving packet, and when read() wakes it delivers all packets accumulated in the meantime at once --
  // low latency at low rates, automatically large batches at high rates, with behavior similar to NAPI.
  // ---------------------------------------------------------------------------
  unsigned int immediate = 1;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCIMMEDIATE, &immediate, "ioctl(BIOCIMMEDIATE)"));

  // ---------------------------------------------------------------------------
  // 7. BIOCSSEESENT = 0 -- capture only the input direction
  //
  // This step is **the key to preventing loops**: the frames we write in are the output direction on this interface,
  // and SEESENT=0 filters them out, leaving only the input frames the host sent from the peer side.
  // ---------------------------------------------------------------------------
  unsigned int see_sent = 0;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSSEESENT, &see_sent, "ioctl(BIOCSSEESENT)"));

  // ---------------------------------------------------------------------------
  // 8. BIOCSRTIMEOUT -- read timeout, used to respond to shutdown
  //
  // The kernel stores tvtohz(tv) - 1 ticks; on this machine hz=100 -> 10 ms resolution;
  // passing {0,0} becomes **blocking forever**, so a non-zero value must be passed here.
  // ---------------------------------------------------------------------------
  ::timeval read_timeout{};
  read_timeout.tv_sec = config.read_timeout_millis / 1000;
  read_timeout.tv_usec = static_cast<__darwin_suseconds_t>(
      (config.read_timeout_millis % 1000) * 1000);
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSRTIMEOUT, &read_timeout, "ioctl(BIOCSRTIMEOUT)"));

  // Deliberately **not setting** BIOCPROMISC: feth's feth_output_common unconditionally delivers frames to the peer
  // and taps them, doing no MAC filtering at all; whether they can be read is determined only by direction. Setting promisc would only add an
  // IFF_PROMISC reference count and a kernel event, purely wasteful.

  // ---------------------------------------------------------------------------
  // 9. Optional optimizations: batch writes and turning off timestamps (both feature-probed)
  // ---------------------------------------------------------------------------
  if (config.try_batch_write) {
    int enable = 1;
    link->batch_write_enabled_ =
        TryOptionalIoctl(link->fd_, kBpfSetBatchWrite, &enable, "BIOCSBATCHWRITE");
  }
  if (config.try_disable_timestamp) {
    int disable = 1;
    link->timestamp_disabled_ =
        TryOptionalIoctl(link->fd_, kBpfSetNoTimestamp, &disable, "BIOCSNOTSTAMP");
  }

  // ---------------------------------------------------------------------------
  // 10. Allocate buffers
  // ---------------------------------------------------------------------------
  link->read_buffer_.resize(link->kernel_buffer_bytes_);
  link->read_frames_.reserve(config.max_frames_per_batch);
  if (link->batch_write_enabled_) {
    link->write_buffer_.resize(kMaxBatchWriteBytes);
  }

  TETHERKITNEXT_INFO_TR(Msg::kNetBpfReady, link->device_path_, link->interface_name_,
                    link->kernel_buffer_bytes_ / 1024, link->max_frame_bytes_,
                    Text(link->batch_write_enabled_ ? Msg::kNetBpfBatchWriteEnabled
                                                    : Msg::kNetBpfBatchWriteUnavailable),
                    Text(link->timestamp_disabled_ ? Msg::kNetBpfTimestampDisabled
                                                   : Msg::kNetBpfTimestampEnabled));

  return link;
}

Result<BpfLink::KernelStats> BpfLink::QueryKernelStats() const {
  ::bpf_stat stats{};
  if (::ioctl(fd_, BIOCGSTATS, &stats) < 0) {
    return std::unexpected(Error::FromErrno(0, "ioctl(BIOCGSTATS)"));
  }
  return KernelStats{.received = stats.bs_recv, .dropped = stats.bs_drop};
}

Result<ReadBatch> BpfLink::ReadFrames() {
  read_frames_.clear();

  // All early returns must carry last_kernel_drops_ rather than the default 0: kernel_drops is a
  // **cumulative** counter, and the consumer differences two adjacent readings. When idle, a read timeout passes through here every 200 ms,
  // and reporting 0 would "zero out" drops that already happened; the next sample with traffic would then re-report the full amount as new,
  // and the difference of the sample in between would also under/overflow on unsigned numbers.
  if (interrupted_.load(std::memory_order_acquire)) {
    return ReadBatch{.kernel_drops = last_kernel_drops_};
  }

  // The length of read() **must exactly equal** the kernel's bd_bufsize, otherwise bpfread returns at the very start with
  // `if (uio_resid(uio) != d->bd_bufsize) return EINVAL`.
  const ssize_t received = ::read(fd_, read_buffer_.data(), read_buffer_.size());
  if (received < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      // Interrupted by a signal or timed out with no data; leave it to the caller to continue looping
      return ReadBatch{.kernel_drops = last_kernel_drops_};
    }
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetBpfReadFailed, device_path_)));
  }
  if (received == 0) {
    return ReadBatch{.kernel_drops = last_kernel_drops_};  // read timeout expired and there were no packets in the meantime
  }

  // ---------------------------------------------------------------------------
  // Iterate BPF records
  //
  // Record layout: [bpf_hdr (bh_hdrlen bytes, including alignment padding)][frame data (bh_caplen bytes)]
  // Offset of the next record = BPF_WORDALIGN(bh_hdrlen + bh_caplen).
  //
  // * Must use bh_hdrlen from the record, not sizeof(struct bpf_hdr). *
  //   Under LP64 sizeof is 20 (18 bytes of content + 2 bytes of compiler padding), while the bh_hdrlen the kernel writes for
  //   DLT_EN10MB is 18 (SIZEOF_BPF_HDR=18,
  //   bif_hdrlen = BPF_WORDALIGN(14 + 18) - 14 = 18). Using 20 would immediately misalign.
  // ---------------------------------------------------------------------------
  const auto total = static_cast<std::size_t>(received);
  std::size_t offset = 0;

  while (offset + kBpfHeaderMinBytes <= total) {
    const std::byte* record = read_buffer_.data() + offset;

    // Read field by field rather than mapping a struct: the record start is only guaranteed 4-byte alignment.
    const std::uint32_t capture_length = LoadLe32(record + offsetof(::bpf_hdr, bh_caplen));
    const std::uint32_t original_length = LoadLe32(record + offsetof(::bpf_hdr, bh_datalen));
    const std::uint32_t header_length = LoadLe16(record + offsetof(::bpf_hdr, bh_hdrlen));

    if (header_length < kBpfHeaderMinBytes) [[unlikely]] {
      TETHERKITNEXT_WARN_TR(Msg::kNetBpfHeaderTooShort, header_length, kBpfHeaderMinBytes);
      break;
    }

    const std::size_t record_bytes = static_cast<std::size_t>(header_length) + capture_length;
    if (offset + record_bytes > total) [[unlikely]] {
      // Record truncated: should not happen normally (the kernel does not write records that cross the end of the buffer).
      TETHERKITNEXT_WARN_TR(Msg::kNetBpfRecordOutOfBounds, offset, record_bytes, total);
      break;
    }

    // Only fully captured frames are forwarded. capture < original means a filter was installed and truncated it,
    // and forwarding a partial frame to USB would only confuse the peer. We install no filter, so this should not trigger.
    if (capture_length == original_length && capture_length >= kMinEthernetFrameBytes &&
        capture_length <= max_frame_bytes_) [[likely]] {
      read_frames_.push_back(FrameView{.data = record + header_length, .length = capture_length});
    } else [[unlikely]] {
      TETHERKITNEXT_TRACE_TR(Msg::kNetBpfSkipOversizedRecord, capture_length, original_length,
                         max_frame_bytes_);
    }

    offset += BPF_WORDALIGN(record_bytes);

    if (read_frames_.size() >= read_frames_.capacity()) {
      // The frame array is full. The remaining records are not processed this time -- is the data still in the kernel buffer? No, it has already been copied out,
      // so it will be lost. Hence max_frames_per_batch must be configured large enough. A warning is issued here rather than staying silent.
      TETHERKITNEXT_WARN_TR(Msg::kNetBpfBatchFrameLimit, read_frames_.capacity(), total - offset);
      break;
    }
  }

  // When BIOCGSTATS occasionally fails, keep the previous cumulative value -- reporting 0 would make the consumer's difference underflow;
  // see the explanation of last_kernel_drops_.
  if (const auto stats = QueryKernelStats()) {
    last_kernel_drops_ = stats->dropped;
  }
  return ReadBatch{
      .frames = read_frames_,
      .kernel_drops = last_kernel_drops_,
  };
}

Result<WriteResult> BpfLink::WriteFrames(FrameBatch frames) {
  if (frames.empty()) {
    return WriteResult{};
  }
  return batch_write_enabled_ ? WriteFramesBatched(frames) : WriteFramesIndividually(frames);
}

Result<WriteResult> BpfLink::WriteFramesIndividually(FrameBatch frames) {
  WriteResult result;
  for (const FrameView& frame : frames) {
    if (frame.length < kMinEthernetFrameBytes || frame.length > max_frame_bytes_) [[unlikely]] {
      ++result.frames_skipped;
      continue;
    }
    const ssize_t written = ::write(fd_, frame.data, frame.length);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      // ENOBUFS is transient (the interface send queue is full) and should not be treated as a fatal error; the caller sees
      // from frames_written < frames.size() that it has not finished writing.
      if (errno == ENOBUFS || errno == EAGAIN) {
        break;
      }
      return std::unexpected(
          Error::FromErrno(0, Tr(Msg::kNetBpfWriteFailed, device_path_, frame.length)));
    }
    ++result.frames_written;
    result.bytes_written += static_cast<std::uint64_t>(written);
  }
  return result;
}

Result<WriteResult> BpfLink::WriteFramesBatched(FrameBatch frames) {
  // The buffer layout of batch writes is **exactly symmetric** to reading: contiguous
  //   [bpf_hdr (bh_hdrlen bytes)][frame data (bh_caplen bytes)]
  // with each entry aligned to BPF_WORDALIGN(bh_hdrlen + bh_caplen).
  //
  // Kernel checks: bh_hdrlen >= 18, bh_caplen == bh_datalen, bh_hdrlen <= remaining length.
  // The timestamp field is ignored and need not be filled.
  //
  // bh_hdrlen here takes 18 (kBpfHeaderMinBytes) rather than sizeof(struct bpf_hdr)=20:
  // both are legal (the part beyond 18 is skipped by the kernel), but 18 is the most compact packing,
  // and also matches the value the kernel writes in the read direction.
  WriteResult result;
  std::size_t cursor = 0;
  std::uint32_t pending_frames = 0;
  std::uint64_t pending_bytes = 0;

  // Writes out the assembled content in one go.
  const auto flush = [&]() -> Status {
    if (cursor == 0) {
      return Ok();
    }
    const ssize_t written = ::write(fd_, write_buffer_.data(), cursor);
    if (written < 0) {
      if (errno == ENOBUFS || errno == EAGAIN || errno == EINTR) {
        // Transient failure: this batch was not sent out, faithfully reflected in the return value.
        cursor = 0;
        pending_frames = 0;
        pending_bytes = 0;
        return Ok();
      }
      return std::unexpected(Error::FromErrno(
          0, Tr(Msg::kNetBpfBatchWriteFailed, device_path_, pending_frames, cursor)));
    }
    result.frames_written += pending_frames;
    result.bytes_written += pending_bytes;
    cursor = 0;
    pending_frames = 0;
    pending_bytes = 0;
    return Ok();
  };

  for (const FrameView& frame : frames) {
    if (frame.length < kMinEthernetFrameBytes || frame.length > max_frame_bytes_) [[unlikely]] {
      ++result.frames_skipped;
      continue;
    }

    const std::size_t record_bytes = kBpfHeaderMinBytes + frame.length;
    const std::size_t aligned_bytes = BPF_WORDALIGN(record_bytes);
    if (cursor + aligned_bytes > write_buffer_.size()) {
      TETHERKITNEXT_RETURN_IF_ERROR(flush());
      if (aligned_bytes > write_buffer_.size()) [[unlikely]] {
        // A single frame exceeds the whole assembly buffer: a configuration error; skip and count.
        ++result.frames_skipped;
        continue;
      }
    }

    std::byte* record = write_buffer_.data() + cursor;
    // The kernel ignores the timestamp field; zeroing it is enough.
    std::memset(record, 0, kBpfHeaderMinBytes);
    StoreLe32(record + offsetof(::bpf_hdr, bh_caplen), frame.length);
    StoreLe32(record + offsetof(::bpf_hdr, bh_datalen), frame.length);
    StoreLe16(record + offsetof(::bpf_hdr, bh_hdrlen),
              static_cast<std::uint16_t>(kBpfHeaderMinBytes));
    std::memcpy(record + kBpfHeaderMinBytes, frame.data, frame.length);

    // Zero the alignment padding, to avoid handing the previous batch's leftover data to the kernel.
    if (aligned_bytes > record_bytes) {
      std::memset(record + record_bytes, 0, aligned_bytes - record_bytes);
    }

    cursor += aligned_bytes;
    ++pending_frames;
    pending_bytes += frame.length;
  }

  TETHERKITNEXT_RETURN_IF_ERROR(flush());
  return result;
}

}  // namespace tetherkitnext::net
