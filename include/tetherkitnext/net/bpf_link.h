// BPF 链路后端：在 feth 的驱动侧接口上直接读写原始以太帧。
//
// ★ 本文件里每一条 ioctl 顺序与参数选择都有源码级理由，改动前务必读完注释 ★
//
// 配置序列（顺序有硬性约束，来自 xnu bsd/net/bpf.c）：
//   1. open("/dev/bpf%d")  —— macOS **没有** /dev/bpf 克隆节点，必须遍历编号。
//   2. BIOCSBLEN           —— **必须在 BIOCSETIF 之前**（bpf.c 里有
//                             `if (d->bd_bif != 0) return EINVAL`）。
//   3. BIOCSHDRCMPLT = 1   —— **必须在 BIOCSBATCHWRITE 之前**，且它决定帧是否
//                             原样透传（见下）。
//   4. BIOCSETIF           —— 绑定到接口，此后才能查 DLT。
//   5. BIOCGDLT            —— 校验是 DLT_EN10MB。
//   6. BIOCIMMEDIATE = 1   —— 决定读取的延迟特性（见下）。
//   7. BIOCSSEESENT = 0    —— 只看 input 方向（见下）。
//   8. BIOCSBATCHWRITE = 1 —— 可选优化，特性探测。
//   9. BIOCSNOTSTAMP = 1   —— 可选优化，特性探测。
//
// 三个最关键的参数选择：
//
//   * **BIOCSHDRCMPLT = 1 是强制的。** hdrcmplt=0 时 bpfwrite 会把用户数据的
//     前 14 字节剥进 sockaddr，再让 ether_frameout 重建帧头 —— 源 MAC 会被
//     驱动改写，转发出去的帧就不是设备发来的原始帧了。hdrcmplt=1 才走
//     DLIL_OUTPUT_FLAGS_RAW，帧原样透传。
//
//   * **BIOCSSEESENT = 0 恰好过滤掉我们自己写进去的帧。** 挂在驱动侧接口上时：
//     主机从系统侧发出的帧在这里是 **input** 方向；我们 write 进去的帧在这里是
//     **output** 方向。SEESENT=0 → bd_direction = BPF_D_IN，只留 input，
//     正是我们要转发到 USB 的那些。不设的话会读到自己刚写的帧，形成回环。
//
//   * **BIOCIMMEDIATE = 1 + 专用线程阻塞 read() 是最优模型，不要用 kqueue。**
//     immediate 下每来一包就 bpf_wakeup，read() 醒来后一次性把期间累积的所有包
//     整批交付 —— 低速时低延迟、高速时自动聚合成大批（行为类似 Linux 的 NAPI），
//     每批只花一次系统调用。kqueue 方案的就绪判据完全一样，却要多一次 kevent()。
//     而如果不开 immediate 又想避免「缓冲填满才返回」，就只能依赖
//     BIOCSRTIMEOUT —— 但它的实际分辨率被 10 ms 的时钟 tick 钉死
//     （内核存 tvtohz(tv)-1，本机 hz=100），延迟不可接受。
//
// 两个必须记住的读取细节：
//
//   * **read() 的缓冲区长度必须精确等于 bd_bufsize**，否则 bpfread 开头就
//     `if (uio_resid(uio) != d->bd_bufsize) return EINVAL`。而 BIOCSBLEN 在
//     超限时**不报错**、静默截到上限并把实际值写回参数，所以必须用写回值。
//
//   * **遍历记录必须用 bh_hdrlen，不能用 sizeof(struct bpf_hdr)。**
//     LP64 下 sizeof 是 20（18 字节内容被补齐），而内核对 DLT_EN10MB 写入的
//     bh_hdrlen 是 18。用 20 会立刻错位。
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

/// BPF 后端的配置参数。
struct BpfConfig {
  /// 内核抓包缓冲区大小。
  ///
  /// 上限是 sysctl debug.bpf_bufsize_cap（macOS 13+ 为 32 MiB），超限会被静默
  /// 截断。内核会为每个描述符分配 **2 份**这么大的缓冲（store + free 双缓冲），
  /// 所以实际 wired 内存占用是这个值的两倍。
  ///
  /// 默认 4 MiB 的推导：1 Gbps 满速下 1514 字节帧约 82 k pps，一次 copyout
  /// 窗口按最坏 10 ms 估算需要缓冲约 1.2 MB；取 4 MiB 留 3 倍余量。再大只是
  /// 浪费 wired 内存，因为 BPF 只有两份缓冲，增大单份并不能改善丢包窗口。
  std::uint32_t kernel_buffer_bytes = 4U * 1024 * 1024;

  /// 单帧长度上限（含以太头）。应等于 接口 MTU + 以太头，不能超过 MTU + 18。
  std::uint32_t max_frame_bytes = 1518;

  /// 一次 ReadFrames 最多解出多少帧。
  ///
  /// 这是 FrameView 数组的容量。4 MiB 缓冲里最多能装约 4Mi/32 ≈ 131 k 个最小
  /// 帧（64 字节帧 + 18 字节头 + 对齐），但实际不会全是最小帧；取 8192 覆盖
  /// 绝大多数批次，超出部分留到下一次 read 之后的解析继续处理。
  std::size_t max_frames_per_batch = 8192;

  /// 是否尝试启用批量写（BIOCSBATCHWRITE，macOS 14+）。
  bool try_batch_write = true;

  /// 是否尝试关闭抓包时间戳（BIOCSNOTSTAMP），省掉每帧一次 microtime()。
  bool try_disable_timestamp = true;

  /// 读超时。决定「完全没有流量时，Interrupt() 后多久能停下来」。
  ///
  /// 注意内核的实际分辨率是 10 ms（存的是 tvtohz(tv) - 1 个 tick，本机
  /// kern.clockrate hz=100），且传 {0,0} 会变成**永久阻塞**。200 ms 既能让
  /// 停机足够及时，又不会产生无谓的唤醒。
  std::uint32_t read_timeout_millis = 200;
};

/// 挂在指定接口上的 BPF 描述符。
class BpfLink final : public LinkBackend {
 public:
  /// 打开并配置一个 BPF 描述符，绑定到 `interface_name`。
  ///
  /// 需要 root：/dev/bpf* 的权限是 0600 root:wheel，而 macOS **没有**
  /// FreeBSD 那样的 access_bpf 组可以走。
  [[nodiscard]] static Result<std::unique_ptr<BpfLink>> Open(std::string_view interface_name,
                                                            const BpfConfig& config);

  // 拷贝与移动已在基类 LinkBackend 中删除，这里显式重申以满足静态检查。
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

  /// 内核实际生效的缓冲区大小（可能被截断到上限）。
  [[nodiscard]] std::uint32_t KernelBufferBytes() const noexcept { return kernel_buffer_bytes_; }

  /// 打开的设备节点路径，用于日志。
  [[nodiscard]] std::string_view DevicePath() const noexcept { return device_path_; }

  /// 内核侧统计：累计收到 / 丢弃的包数。
  struct KernelStats {
    std::uint64_t received = 0;
    std::uint64_t dropped = 0;
  };

  [[nodiscard]] Result<KernelStats> QueryKernelStats() const;

 private:
  BpfLink() = default;

  /// 逐帧 write()，批量写不可用时的回落路径。
  [[nodiscard]] Result<WriteResult> WriteFramesIndividually(FrameBatch frames);

  /// 一次 write() 发多帧（BIOCSBATCHWRITE）。
  [[nodiscard]] Result<WriteResult> WriteFramesBatched(FrameBatch frames);

  int fd_ = -1;
  std::string device_path_;
  std::string interface_name_;

  std::uint32_t kernel_buffer_bytes_ = 0;
  std::uint32_t max_frame_bytes_ = 0;
  bool batch_write_enabled_ = false;
  bool timestamp_disabled_ = false;

  /// 读缓冲。长度必须精确等于 kernel_buffer_bytes_（bpfread 的硬性要求）。
  std::vector<std::byte> read_buffer_;
  /// 本次批次解出的帧视图，指向 read_buffer_ 内部。
  std::vector<FrameView> read_frames_;
  /// 最近一次成功读到的内核累计丢包数（bs_drop）。
  ///
  /// BIOCGSTATS 偶发失败时**沿用上次的值**而不是报 0：bs_drop 是累计计数，
  /// 消费方拿它做「当前 − 上次」的差分，报 0 会让差分在无符号数上下溢出，
  /// 打出一条天文数字的「内核丢包」。只被读线程访问，无需原子。
  std::uint64_t last_kernel_drops_ = 0;
  /// 批量写的组装缓冲。
  std::vector<std::byte> write_buffer_;

  /// 停机标志。Interrupt() 置位后 ReadFrames 会在一个读超时周期内返回空批次。
  ///
  /// 唤醒机制的选择：BPF 的 read() 是阻塞的，要打断它有三条路 ——
  ///   (a) 发信号让 BPF_SLEEP（带 PCATCH）返回 EINTR；
  ///   (b) 关掉 fd（有 use-after-close 竞态，不可取）；
  ///   (c) 设一个适中的 BIOCSRTIMEOUT，让 read() 周期性醒来检查本标志。
  /// 选 (c)：不需要在进程里安装信号处理器，也没有竞态。BIOCIMMEDIATE=1 保证
  /// 有包时仍然立即返回，读超时只影响「完全没流量时多久能响应停机」。
  std::atomic<bool> interrupted_{false};
};

}  // namespace tetherkitnext::net
