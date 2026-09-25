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

/// 遍历 /dev/bpf%d 的上限。
///
/// 内核在打开当前最后一个节点时会按需再造一个（bpfopen 里
/// `if (minor(dev) == nbpfilter - 1) bpf_make_dev_t(...)`），上限由 sysctl
/// debug.bpf_maxdevices 控制（本机 256）。这里取 256 与之对齐。
constexpr int kMaxBpfDeviceIndex = 256;

/// 批量写时一次 write() 的字节上限。
///
/// 内核允许的绝对上限是 BPF_WRITE_MAX = 16 MiB，但没必要用那么大：
/// 256 KiB 已能装下上百个满帧，单次系统调用的固定开销早就被摊薄了，
/// 而更大的组装缓冲只会恶化 L2 缓存命中。
constexpr std::size_t kMaxBatchWriteBytes = std::size_t{256} * 1024;

/// 一次 ioctl，失败时把 errno 与调用名包进 Error。
[[nodiscard]] Status CallIoctl(int fd, unsigned long request, void* argument,
                               std::string_view what) {
  if (::ioctl(fd, request, argument) < 0) {
    return std::unexpected(Error::FromErrno(0, std::string{what}));
  }
  return Ok();
}

/// 尝试一个**可选**的 ioctl：失败不算错误，只返回是否成功。
[[nodiscard]] bool TryOptionalIoctl(int fd, unsigned long request, void* argument,
                                    std::string_view what) {
  if (::ioctl(fd, request, argument) < 0) {
    // ENOTTY / EINVAL 表示当前 macOS 版本不支持这个私有 ioctl，属于预期情况。
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
  // 1. 打开 /dev/bpf%d
  //
  // macOS **没有** /dev/bpf 克隆节点（实测 ls /dev/bpf → No such file），
  // 必须逐个编号尝试：EBUSY 表示该 minor 已被别的进程占用，试下一个；
  // ENOENT 表示已到内核当前造出的节点上限。
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
      continue;  // 被别的抓包程序占了，换下一个
    }
    if (last_errno == ENOENT) {
      break;  // 已经到上限，不会再有更大编号的节点
    }
    if (last_errno == EACCES || last_errno == EPERM) {
      return std::unexpected(
          Error::FromErrno(last_errno, Tr(Msg::kNetBpfOpenDenied, path)));
    }
    // 其它 errno：继续试下一个编号，最后统一报错。
  }

  if (link->fd_ < 0) {
    return std::unexpected(
        Error::FromErrno(last_errno, Tr(Msg::kNetBpfNoDeviceAvailable)));
  }

  // ---------------------------------------------------------------------------
  // 2. BIOCSBLEN —— **必须在 BIOCSETIF 之前**
  //
  // bpf.c 里：`if (d->bd_bif != 0 || (d->bd_flags & BPF_DETACHING)) return EINVAL`。
  // 而且超限时**不报错**，静默截到 BPF_BUFSIZE_CAP 并通过 _IOWR 把实际生效值
  // 写回参数 —— 所以后面每次 read() 都必须用这个写回值当长度。
  // ---------------------------------------------------------------------------
  auto buffer_bytes = static_cast<unsigned int>(config.kernel_buffer_bytes);
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSBLEN, &buffer_bytes, "ioctl(BIOCSBLEN)"));
  link->kernel_buffer_bytes_ = buffer_bytes;
  if (buffer_bytes != config.kernel_buffer_bytes) {
    TETHERKITNEXT_INFO_TR(Msg::kNetBpfBufferClamped, config.kernel_buffer_bytes, buffer_bytes);
  }

  // ---------------------------------------------------------------------------
  // 3. BIOCSHDRCMPLT = 1 —— **强制**，且必须在 BIOCSBATCHWRITE 之前
  //
  // hdrcmplt=0 时 bpfwrite 会剥掉前 14 字节重建帧头（源 MAC 被驱动改写）；
  // =1 才走 DLIL_OUTPUT_FLAGS_RAW 原样透传。批量写还硬性要求它是 1。
  //
  // ⚠️ 而且在 feth 上后果比「帧头被改写」严重得多：**不设这个 ioctl，write()
  // 直接返回 ENXIO**（if_fake 的输出路径处理不了 AF_UNSPEC 那条重建分支）。
  // 对照实验见 AGENTS.md 第 7 节第 14 条。排查 ENXIO 时别去怀疑接口名。
  // ---------------------------------------------------------------------------
  int header_complete = 1;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSHDRCMPLT, &header_complete, "ioctl(BIOCSHDRCMPLT)"));

  // ---------------------------------------------------------------------------
  // 4. BIOCSETIF —— 绑定到接口
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
  // 5. BIOCGDLT —— 校验数据链路类型
  //
  // feth 的 bpfattach 用的是 DLT_EN10MB，所以这里一定是 1；校验它是为了防止
  // 有人误把 BPF 绑到别的接口类型上，那样帧格式假设就全错了。
  // ---------------------------------------------------------------------------
  unsigned int data_link_type = 0;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCGDLT, &data_link_type, "ioctl(BIOCGDLT)"));
  if (data_link_type != DLT_EN10MB) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetBpfWrongDataLinkType, interface_name, data_link_type, DLT_EN10MB)));
  }

  // ---------------------------------------------------------------------------
  // 6. BIOCIMMEDIATE = 1 —— 立即投递
  //
  // 不开的话 read() 只在缓冲**填满**时才返回；用 4 MiB 缓冲的话延迟灾难性地高。
  // 开了之后每来一包就唤醒，read() 醒来时一次性交付期间累积的全部包 ——
  // 低速低延迟、高速自动大批量，行为类似 NAPI。
  // ---------------------------------------------------------------------------
  unsigned int immediate = 1;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCIMMEDIATE, &immediate, "ioctl(BIOCIMMEDIATE)"));

  // ---------------------------------------------------------------------------
  // 7. BIOCSSEESENT = 0 —— 只抓 input 方向
  //
  // 这一步是**防回环的关键**：我们 write 进去的帧在本接口上是 output 方向，
  // SEESENT=0 会把它们滤掉，只留下主机从 peer 侧发来的 input 帧。
  // ---------------------------------------------------------------------------
  unsigned int see_sent = 0;
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSSEESENT, &see_sent, "ioctl(BIOCSSEESENT)"));

  // ---------------------------------------------------------------------------
  // 8. BIOCSRTIMEOUT —— 读超时，用于响应停机
  //
  // 内核存的是 tvtohz(tv) - 1 个 tick，本机 hz=100 → 分辨率 10 ms；
  // 传 {0,0} 会变成**永久阻塞**，因此这里必须传非零值。
  // ---------------------------------------------------------------------------
  ::timeval read_timeout{};
  read_timeout.tv_sec = config.read_timeout_millis / 1000;
  read_timeout.tv_usec = static_cast<__darwin_suseconds_t>(
      (config.read_timeout_millis % 1000) * 1000);
  TETHERKITNEXT_RETURN_IF_ERROR(
      CallIoctl(link->fd_, BIOCSRTIMEOUT, &read_timeout, "ioctl(BIOCSRTIMEOUT)"));

  // 刻意**不设** BIOCPROMISC：feth 的 feth_output_common 无条件把帧投给 peer
  // 并 tap，不做任何 MAC 过滤，能否读到只由方向决定。设 promisc 只会多一个
  // IFF_PROMISC 引用计数和一个内核事件，纯属浪费。

  // ---------------------------------------------------------------------------
  // 9. 可选优化：批量写与关闭时间戳（都做特性探测）
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
  // 10. 分配缓冲
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

  // 所有提前返回都必须带上 last_kernel_drops_ 而不是默认的 0：kernel_drops 是
  // **累计**计数，消费方拿相邻两次做差分。空闲时读超时每 200 ms 就走一次这里，
  // 报 0 会把已发生的丢包「清零」，下一个有流量的采样又把全量当成新增重报，
  // 中间那个采样的差分还会在无符号数上下溢。
  if (interrupted_.load(std::memory_order_acquire)) {
    return ReadBatch{.kernel_drops = last_kernel_drops_};
  }

  // read() 的长度**必须精确等于**内核的 bd_bufsize，否则 bpfread 一开头就
  // `if (uio_resid(uio) != d->bd_bufsize) return EINVAL`。
  const ssize_t received = ::read(fd_, read_buffer_.data(), read_buffer_.size());
  if (received < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      // 被信号打断或超时无数据，交给调用方继续循环
      return ReadBatch{.kernel_drops = last_kernel_drops_};
    }
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetBpfReadFailed, device_path_)));
  }
  if (received == 0) {
    return ReadBatch{.kernel_drops = last_kernel_drops_};  // 读超时到期且期间无包
  }

  // ---------------------------------------------------------------------------
  // 遍历 BPF 记录
  //
  // 记录布局：[bpf_hdr（bh_hdrlen 字节，含对齐填充）][帧数据（bh_caplen 字节）]
  // 下一条记录的偏移 = BPF_WORDALIGN(bh_hdrlen + bh_caplen)。
  //
  // ★ 必须用记录里的 bh_hdrlen，不能用 sizeof(struct bpf_hdr)。★
  //   LP64 下 sizeof 是 20（18 字节内容 + 2 字节编译器填充），而内核对
  //   DLT_EN10MB 写入的 bh_hdrlen 是 18（SIZEOF_BPF_HDR=18，
  //   bif_hdrlen = BPF_WORDALIGN(14 + 18) - 14 = 18）。用 20 会立刻错位。
  // ---------------------------------------------------------------------------
  const auto total = static_cast<std::size_t>(received);
  std::size_t offset = 0;

  while (offset + kBpfHeaderMinBytes <= total) {
    const std::byte* record = read_buffer_.data() + offset;

    // 逐字段读取而非结构体映射：记录起始只保证 4 字节对齐。
    const std::uint32_t capture_length = LoadLe32(record + offsetof(::bpf_hdr, bh_caplen));
    const std::uint32_t original_length = LoadLe32(record + offsetof(::bpf_hdr, bh_datalen));
    const std::uint32_t header_length = LoadLe16(record + offsetof(::bpf_hdr, bh_hdrlen));

    if (header_length < kBpfHeaderMinBytes) [[unlikely]] {
      TETHERKITNEXT_WARN_TR(Msg::kNetBpfHeaderTooShort, header_length, kBpfHeaderMinBytes);
      break;
    }

    const std::size_t record_bytes = static_cast<std::size_t>(header_length) + capture_length;
    if (offset + record_bytes > total) [[unlikely]] {
      // 记录被截断：正常情况下不该发生（内核不会写出跨越缓冲末尾的记录）。
      TETHERKITNEXT_WARN_TR(Msg::kNetBpfRecordOutOfBounds, offset, record_bytes, total);
      break;
    }

    // 只有完整捕获的帧才转发。capture < original 说明装了过滤器且截断了，
    // 转发一个残帧到 USB 只会让对端困惑。我们不装过滤器，所以这里不该触发。
    if (capture_length == original_length && capture_length >= kMinEthernetFrameBytes &&
        capture_length <= max_frame_bytes_) [[likely]] {
      read_frames_.push_back(FrameView{.data = record + header_length, .length = capture_length});
    } else [[unlikely]] {
      TETHERKITNEXT_TRACE_TR(Msg::kNetBpfSkipOversizedRecord, capture_length, original_length,
                         max_frame_bytes_);
    }

    offset += BPF_WORDALIGN(record_bytes);

    if (read_frames_.size() >= read_frames_.capacity()) {
      // 帧数组满了。剩余记录本次不处理 —— 数据仍在内核缓冲里？不，已经 copyout
      // 了，会丢。因此 max_frames_per_batch 必须配得足够大。这里出警告而非静默。
      TETHERKITNEXT_WARN_TR(Msg::kNetBpfBatchFrameLimit, read_frames_.capacity(), total - offset);
      break;
    }
  }

  // BIOCGSTATS 偶发失败时沿用上次的累计值 —— 报 0 会让消费方的差分下溢，
  // 见 last_kernel_drops_ 的说明。
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
      // ENOBUFS 是暂时性的（接口发送队列满），不该当致命错误；调用方看
      // frames_written < frames.size() 就知道有没写完。
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
  // 批量写的缓冲布局与读取**完全对称**：连续的
  //   [bpf_hdr（bh_hdrlen 字节）][帧数据（bh_caplen 字节）]
  // 每条按 BPF_WORDALIGN(bh_hdrlen + bh_caplen) 对齐。
  //
  // 内核校验：bh_hdrlen >= 18、bh_caplen == bh_datalen、bh_hdrlen <= 剩余长度。
  // 时间戳字段被忽略，不必填。
  //
  // 这里 bh_hdrlen 取 18（kBpfHeaderMinBytes）而不是 sizeof(struct bpf_hdr)=20：
  // 两者都合法（超过 18 的部分被内核跳过），但 18 是最紧凑的打包，
  // 也与内核在读方向写出的值一致。
  WriteResult result;
  std::size_t cursor = 0;
  std::uint32_t pending_frames = 0;
  std::uint64_t pending_bytes = 0;

  // 把已组装的内容一次性写出。
  const auto flush = [&]() -> Status {
    if (cursor == 0) {
      return Ok();
    }
    const ssize_t written = ::write(fd_, write_buffer_.data(), cursor);
    if (written < 0) {
      if (errno == ENOBUFS || errno == EAGAIN || errno == EINTR) {
        // 暂时性失败：这一批没发出去，如实反映在返回值里。
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
        // 单帧就超过整个组装缓冲：配置错误，跳过并计数。
        ++result.frames_skipped;
        continue;
      }
    }

    std::byte* record = write_buffer_.data() + cursor;
    // 时间戳字段内核忽略，清零即可。
    std::memset(record, 0, kBpfHeaderMinBytes);
    StoreLe32(record + offsetof(::bpf_hdr, bh_caplen), frame.length);
    StoreLe32(record + offsetof(::bpf_hdr, bh_datalen), frame.length);
    StoreLe16(record + offsetof(::bpf_hdr, bh_hdrlen),
              static_cast<std::uint16_t>(kBpfHeaderMinBytes));
    std::memcpy(record + kBpfHeaderMinBytes, frame.data, frame.length);

    // 对齐填充清零，避免把上一批的残留数据交给内核。
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
