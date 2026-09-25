// Darwin 私有 ABI 声明。
//
// 本文件声明的东西**不在**公开 macOS SDK 里，但驱动必须用到。全部定义都从
// Apple 开源的 xnu 源码抄来，并在本机用 static_assert + 实测 ioctl 编号核对过。
//
// 为什么敢用私有 ABI（以及风险评估）：
//   * `net/if_fake_var.h` 在 xnu-7195(macOS 11) → xnu-12377(macOS 26) 的所有
//     发布 tag 下文件内容完全相同（md5 一致），15 年来未变动过一个字节。
//     `ifconfig fethN peer fethM` 走的就是这套 ABI，Apple 自己的 ifconfig
//     依赖它，因此它实际上是被冻结的。
//   * `struct ifdrv` 同理，且它的大小直接参与 _IOW 宏计算 ioctl 编号 ——
//     一旦大小算错，ioctl 号就错，内核会返回 ENOTTY 而不会做危险的事。
//     下面用 static_assert 把大小钉死为 40。
//   * BPF 的私有 ioctl（BIOCSBATCHWRITE / BIOCSNOTSTAMP）是**可选优化**，
//     一律做运行时特性探测，失败就回落到通用路径，不影响功能正确性。
//     macOS 26 把它们从 net/bpf.h 挪到了 net/bpf_private.h（SDK 未提供该文件）。
//
// 一句话：功能性 ABI（feth peer）经过 15 年验证且有 static_assert 兜底；
// 优化性 ABI（BPF 批量写）做特性探测。两者都不会静默走错路径。
#pragma once

#include <net/bpf.h>
#include <net/if.h>
#include <sys/ioccom.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <cstddef>
#include <cstdint>

#include "tetherkitnext/common/i18n.h"

namespace tetherkitnext::net {

// =============================================================================
// struct ifdrv —— SIOCSDRVSPEC / SIOCGDRVSPEC 的参数
//
// 出处：xnu/bsd/net/if.h（带 #pragma pack(4)，但在 LP64/arm64 上不改变布局）。
// SDK 的 net/if.h 里没有这个结构，只有引用它的 ioctl 宏定义。
// =============================================================================

/// 驱动私有 ioctl 的参数块。
struct IfDrv {
  char ifd_name[IFNAMSIZ];  ///< 目标接口名。
  unsigned long ifd_cmd;    ///< 驱动私有命令码（对 feth 是 IF_FAKE_S_CMD_*）。
  std::size_t ifd_len;      ///< ifd_data 指向的数据长度。
  void* ifd_data;           ///< 命令参数。
};

// 大小必须正好 40 字节：它参与 _IOW('i', 123, struct ifdrv) 的 ioctl 编号计算，
// 算错的话得到的是一个不存在的 ioctl 号。实测 SIOCSDRVSPEC = 0x8028697b。
static_assert(sizeof(IfDrv) == 40, "struct ifdrv 必须是 40 字节，否则 ioctl 编号会算错");
static_assert(offsetof(IfDrv, ifd_cmd) == 16);
static_assert(offsetof(IfDrv, ifd_len) == 24);
static_assert(offsetof(IfDrv, ifd_data) == 32);

/// 设置驱动私有参数。等价于 SDK 的 SIOCSDRVSPEC，但用我们自己的 IfDrv 计算。
inline constexpr unsigned long kSetDriverSpec = _IOW('i', 123, IfDrv);
/// 读取驱动私有参数。
inline constexpr unsigned long kGetDriverSpec = _IOWR('i', 123, IfDrv);

static_assert(kSetDriverSpec == 0x8028'697BUL, "SIOCSDRVSPEC 编号与实测值不符");
static_assert(kGetDriverSpec == 0xC028'697BUL, "SIOCGDRVSPEC 编号与实测值不符");

// =============================================================================
// if_fake（feth）私有 ABI
//
// 出处：xnu/bsd/net/if_fake_var.h
// =============================================================================

/// feth 的驱动名（if_clone 名字，创建时填进 ifr_name 作为通配前缀）。
inline constexpr const char* kFethCloneName = "feth";

/// SIOCSDRVSPEC 的命令码。
enum class FethSetCommand : unsigned long {
  kNone = 0,
  kSetPeer = 1,           ///< 配对 / 解绑 peer。
  kSetMedia = 2,
  kSetDequeueStall = 3,
};

/// SIOCGDRVSPEC 的命令码。
enum class FethGetCommand : unsigned long {
  kNone = 0,
  kGetPeer = 1,
};

/// if_fake_media 的媒体类型列表上限。
inline constexpr std::size_t kFethMediaListMax = 27;

/// SIOCSDRVSPEC / SIOCGDRVSPEC 对 feth 的参数块。
///
/// 布局必须与 xnu 的 struct if_fake_request 逐字节一致：
///   uint64_t iffr_reserved[4];            // 32 字节，**必须全零**
///   union {                               // 128 字节
///     char     iffru_buf[128];
///     struct   if_fake_media iffru_media; // 也是 128 字节
///     char     iffru_peer_name[IFNAMSIZ];
///     uint32_t iffru_dequeue_stall;
///   };
struct FethRequest {
  /// 保留字段。内核会校验它**必须全为 0**，非零直接返回 EINVAL。
  std::uint64_t reserved[4];

  union {
    char buffer[128];
    struct {
      std::int32_t current;
      std::uint32_t count;
      std::uint32_t media_reserved[3];
      std::int32_t list[kFethMediaListMax];
    } media;
    /// peer 接口名。写空串（首字节 '\0'）表示解绑。
    char peer_name[IFNAMSIZ];
    std::uint32_t dequeue_stall;
  } u;
};

static_assert(sizeof(FethRequest) == 160, "struct if_fake_request 必须是 160 字节");
static_assert(offsetof(FethRequest, u) == 32);
static_assert(sizeof(FethRequest::u) == 128);
static_assert(alignof(FethRequest) == 8);

// =============================================================================
// BPF 私有常量与 ioctl
//
// 出处：xnu/bsd/net/bpf.h 的 PRIVATE 段（macOS 26 起挪到 bsd/net/bpf_private.h）。
// SDK 均未提供，全部做运行时特性探测。
// =============================================================================

/// 启用批量写：一次 write() 发送多帧。
///
/// 内核实现出现在 macOS 14 / xnu-10063 起；macOS 13 及更早完全没有。
/// 因此必须探测：ioctl 返回 ENOTTY / EINVAL 就回落到逐帧 write。
///
/// 前置条件：BIOCSHDRCMPLT 必须已设为 1，且不得设置过 BIOCSETTC。
inline constexpr unsigned long kBpfSetBatchWrite = _IOW('B', 143, int);
static_assert(kBpfSetBatchWrite == 0x8004'428FUL, "BIOCSBATCHWRITE 编号与实测值不符");

/// 关闭抓包时间戳，让 catchpacket 跳过 microtime() 调用。
///
/// 我们不需要时间戳（帧直接转发给 USB），关掉能省掉每帧一次时钟读取。
/// 同样做特性探测，失败无害。
inline constexpr unsigned long kBpfSetNoTimestamp = _IOW('B', 145, int);
static_assert(kBpfSetNoTimestamp == 0x8004'4291UL, "BIOCSNOTSTAMP 编号与实测值不符");

/// 显式设置抓包方向（BIOCSSEESENT 的更精确版本）。
inline constexpr unsigned long kBpfSetDirection = _IOW('B', 138, unsigned int);

/// 抓包方向取值。
inline constexpr unsigned int kBpfDirectionNone = 0;
inline constexpr unsigned int kBpfDirectionIn = 0x1;
inline constexpr unsigned int kBpfDirectionOut = 0x2;
inline constexpr unsigned int kBpfDirectionInOut = kBpfDirectionIn | kBpfDirectionOut;

/// bpfwrite 允许的超长余量。
///
/// 内核检查是 `(len - hlen) > (ifp->if_mtu + BPF_WRITE_LEEWAY)` → EMSGSIZE。
/// 在 BIOCSHDRCMPLT=1（我们的用法）下 hlen == 0，所以**整帧长度（含 14 字节
/// 以太头）必须 <= 接口 MTU + 18**。MTU=1500 时上限 1518，刚好容得下标准
/// 1514 帧和带 VLAN 标签的 1518 帧。
inline constexpr std::uint32_t kBpfWriteLeeway = 18;

/// BIOCSBLEN 的上下限。
///
/// macOS 13（xnu-8792）起上限是 BPF_BUFSIZE_CAP，本机通过只读 sysctl
/// debug.bpf_bufsize_cap 暴露为 32 MiB。**超限不报错**，而是静默截到上限并
/// 通过 _IOWR 把实际生效值写回参数 —— 所以必须使用写回值，见 bpf_link.cc。
inline constexpr std::uint32_t kBpfMinBufferBytes = 32;

/// BPF 记录头的最小长度。
///
/// 关键陷阱：`sizeof(struct bpf_hdr)` 在 LP64 上是 **20**（18 字节内容被编译器
/// 补齐到 20），而内核实际写入的 `bh_hdrlen` 对 DLT_EN10MB 是 **18**
/// （SIZEOF_BPF_HDR=18，bif_hdrlen = BPF_WORDALIGN(14+18) - 14 = 18）。
/// **读取时必须用记录里的 bh_hdrlen，用 sizeof 会立刻错位。**
inline constexpr std::uint32_t kBpfHeaderMinBytes = 18;

static_assert(sizeof(struct bpf_hdr) == 20,
              "LP64 下 sizeof(struct bpf_hdr) 应为 20（内容 18 字节 + 2 字节填充）");
static_assert(sizeof(struct BPF_TIMEVAL) == 8,
              "LP64 下 BPF 时间戳是 timeval32（8 字节），不是 64 位 timeval");

// =============================================================================
// feth 创建期会被快照的 sysctl
//
// 这些开关在 feth_clone_create() 那一刻从 sysctl 读进接口的私有标志位，
// **创建之后再改 sysctl 无效**。因此必须在创建 feth 之前校验它们。
// =============================================================================

/// 一个必须为特定值的 feth sysctl。
///
/// `why` 存的是文案标识而不是现成的字符串：这张表是 constexpr 的，而「为什么」
/// 要按用户当前语言渲染，只能推迟到出错那一刻再查表。
struct RequiredFethSysctl {
  const char* name;
  std::int32_t required_value;
  Msg why;
};

/// 创建 feth 前必须校验的 sysctl 清单。
///
/// 这些值如果不对，我们从 BPF 读到的帧就不是「干净的以太帧」：
inline constexpr RequiredFethSysctl kRequiredFethSysctls[] = {
    {"net.link.fake.hwcsum", 0, Msg::kNetSysctlWhyHwcsum},
    {"net.link.fake.fcs", 0, Msg::kNetSysctlWhyFcs},
    {"net.link.fake.tso_support", 0, Msg::kNetSysctlWhyTso},
    {"net.link.fake.lro", 0, Msg::kNetSysctlWhyLro},
    {"net.link.fake.trailer_length", 0, Msg::kNetSysctlWhyTrailer},
    {"net.link.fake.separate_frame_header", 0, Msg::kNetSysctlWhySeparateHeader},
};

}  // namespace tetherkitnext::net
