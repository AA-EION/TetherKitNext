// Darwin private ABI declarations.
//
// The things declared in this file are **not** in the public macOS SDK, yet the driver must use them. All definitions are copied
// from Apple's open-source xnu sources, and verified locally with static_assert + measured ioctl numbers.
//
// Why dare to use a private ABI (and the risk assessment):
//   * `net/if_fake_var.h` is byte-for-byte identical (same md5) across all release tags from
//     xnu-7195 (macOS 11) to xnu-12377 (macOS 26); it has not changed a single byte in 15 years.
//     `ifconfig fethN peer fethM` uses exactly this ABI, and Apple's own ifconfig
//     depends on it, so it is effectively frozen.
//   * The same goes for `struct ifdrv`, and its size directly participates in the _IOW macro's computation of the ioctl number --
//     if the size is computed wrongly, the ioctl number is wrong, and the kernel returns ENOTTY without doing anything dangerous.
//     The static_assert below pins the size to 40.
//   * BPF's private ioctls (BIOCSBATCHWRITE / BIOCSNOTSTAMP) are **optional optimizations**,
//     always feature-probed at runtime, falling back to the generic path on failure, without affecting functional correctness.
//     macOS 26 moved them from net/bpf.h to net/bpf_private.h (a file the SDK does not provide).
//
// In one sentence: the functional ABI (feth peer) has been validated for 15 years and is backstopped by static_assert;
// the optimization ABI (BPF batch write) is feature-probed. Neither can silently take the wrong path.
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
// struct ifdrv -- the argument of SIOCSDRVSPEC / SIOCGDRVSPEC
//
// Source: xnu/bsd/net/if.h (with #pragma pack(4), which does not change the layout on LP64/arm64).
// The SDK's net/if.h does not have this struct, only the ioctl macro definitions that reference it.
// =============================================================================

/// Argument block of driver-private ioctls.
struct IfDrv {
  char ifd_name[IFNAMSIZ];  ///< Target interface name.
  unsigned long ifd_cmd;    ///< Driver-private command code (IF_FAKE_S_CMD_* for feth).
  std::size_t ifd_len;      ///< Length of the data pointed to by ifd_data.
  void* ifd_data;           ///< Command argument.
};

// The size must be exactly 40 bytes: it participates in the ioctl number computation of _IOW('i', 123, struct ifdrv),
// and if computed wrongly the result is a nonexistent ioctl number. Measured SIOCSDRVSPEC = 0x8028697b.
static_assert(sizeof(IfDrv) == 40, "struct ifdrv 必须是 40 字节，否则 ioctl 编号会算错");
static_assert(offsetof(IfDrv, ifd_cmd) == 16);
static_assert(offsetof(IfDrv, ifd_len) == 24);
static_assert(offsetof(IfDrv, ifd_data) == 32);

/// Sets driver-private parameters. Equivalent to the SDK's SIOCSDRVSPEC, but computed with our own IfDrv.
inline constexpr unsigned long kSetDriverSpec = _IOW('i', 123, IfDrv);
/// Reads driver-private parameters.
inline constexpr unsigned long kGetDriverSpec = _IOWR('i', 123, IfDrv);

static_assert(kSetDriverSpec == 0x8028'697BUL, "SIOCSDRVSPEC 编号与实测值不符");
static_assert(kGetDriverSpec == 0xC028'697BUL, "SIOCGDRVSPEC 编号与实测值不符");

// =============================================================================
// if_fake (feth) private ABI
//
// Source: xnu/bsd/net/if_fake_var.h
// =============================================================================

/// The driver name of feth (the if_clone name, filled into ifr_name as a wildcard prefix at creation).
inline constexpr const char* kFethCloneName = "feth";

/// Command codes for SIOCSDRVSPEC.
enum class FethSetCommand : unsigned long {
  kNone = 0,
  kSetPeer = 1,           ///< Pair / unpair the peer.
  kSetMedia = 2,
  kSetDequeueStall = 3,
};

/// Command codes for SIOCGDRVSPEC.
enum class FethGetCommand : unsigned long {
  kNone = 0,
  kGetPeer = 1,
};

/// Upper limit of the media type list of if_fake_media.
inline constexpr std::size_t kFethMediaListMax = 27;

/// Argument block of SIOCSDRVSPEC / SIOCGDRVSPEC for feth.
///
/// The layout must match xnu's struct if_fake_request byte for byte:
///   uint64_t iffr_reserved[4];            // 32 bytes, **must be all zero**
///   union {                               // 128 bytes
///     char     iffru_buf[128];
///     struct   if_fake_media iffru_media; // also 128 bytes
///     char     iffru_peer_name[IFNAMSIZ];
///     uint32_t iffru_dequeue_stall;
///   };
struct FethRequest {
  /// Reserved field. The kernel verifies it **must be all 0**; non-zero returns EINVAL directly.
  std::uint64_t reserved[4];

  union {
    char buffer[128];
    struct {
      std::int32_t current;
      std::uint32_t count;
      std::uint32_t media_reserved[3];
      std::int32_t list[kFethMediaListMax];
    } media;
    /// Peer interface name. Writing an empty string (first byte '\0') means unpair.
    char peer_name[IFNAMSIZ];
    std::uint32_t dequeue_stall;
  } u;
};

static_assert(sizeof(FethRequest) == 160, "struct if_fake_request 必须是 160 字节");
static_assert(offsetof(FethRequest, u) == 32);
static_assert(sizeof(FethRequest::u) == 128);
static_assert(alignof(FethRequest) == 8);

// =============================================================================
// BPF private constants and ioctls
//
// Source: the PRIVATE section of xnu/bsd/net/bpf.h (moved to bsd/net/bpf_private.h starting with macOS 26).
// Not provided by the SDK; all are feature-probed at runtime.
// =============================================================================

/// Enables batch writes: send multiple frames with one write().
///
/// The kernel implementation appeared in macOS 14 / xnu-10063; macOS 13 and earlier have nothing at all.
/// Hence probing is required: if the ioctl returns ENOTTY / EINVAL, fall back to per-frame write.
///
/// Precondition: BIOCSHDRCMPLT must already be set to 1, and BIOCSETTC must not have been set.
inline constexpr unsigned long kBpfSetBatchWrite = _IOW('B', 143, int);
static_assert(kBpfSetBatchWrite == 0x8004'428FUL, "BIOCSBATCHWRITE 编号与实测值不符");

/// Turns off capture timestamps so catchpacket skips the microtime() call.
///
/// We do not need timestamps (frames are forwarded directly to USB); turning them off saves one clock read per frame.
/// Also feature-probed; failure is harmless.
inline constexpr unsigned long kBpfSetNoTimestamp = _IOW('B', 145, int);
static_assert(kBpfSetNoTimestamp == 0x8004'4291UL, "BIOCSNOTSTAMP 编号与实测值不符");

/// Explicitly sets the capture direction (a more precise version of BIOCSSEESENT).
inline constexpr unsigned long kBpfSetDirection = _IOW('B', 138, unsigned int);

/// Capture direction values.
inline constexpr unsigned int kBpfDirectionNone = 0;
inline constexpr unsigned int kBpfDirectionIn = 0x1;
inline constexpr unsigned int kBpfDirectionOut = 0x2;
inline constexpr unsigned int kBpfDirectionInOut = kBpfDirectionIn | kBpfDirectionOut;

/// The oversize allowance permitted by bpfwrite.
///
/// The kernel check is `(len - hlen) > (ifp->if_mtu + BPF_WRITE_LEEWAY)` -> EMSGSIZE.
/// Under BIOCSHDRCMPLT=1 (our usage) hlen == 0, so the **whole frame length (including the 14-byte
/// Ethernet header) must be <= interface MTU + 18**. With MTU=1500 the cap is 1518, which exactly fits a standard
/// 1514 frame and a 1518 frame with a VLAN tag.
inline constexpr std::uint32_t kBpfWriteLeeway = 18;

/// Upper and lower bounds of BIOCSBLEN.
///
/// Starting with macOS 13 (xnu-8792) the upper bound is BPF_BUFSIZE_CAP, exposed on this machine through the read-only sysctl
/// debug.bpf_bufsize_cap as 32 MiB. **Exceeding it does not report an error**; it silently clamps to the cap and
/// writes the actually effective value back through _IOWR -- so the written-back value must be used; see bpf_link.cc.
inline constexpr std::uint32_t kBpfMinBufferBytes = 32;

/// Minimum length of a BPF record header.
///
/// Key pitfall: `sizeof(struct bpf_hdr)` is **20** on LP64 (the 18 bytes of content are padded by the compiler
/// to 20), while the `bh_hdrlen` the kernel actually writes for DLT_EN10MB is **18**
/// (SIZEOF_BPF_HDR=18, bif_hdrlen = BPF_WORDALIGN(14+18) - 14 = 18).
/// **When reading, the bh_hdrlen in the record must be used; using sizeof misaligns immediately.**
inline constexpr std::uint32_t kBpfHeaderMinBytes = 18;

static_assert(sizeof(struct bpf_hdr) == 20,
              "LP64 下 sizeof(struct bpf_hdr) 应为 20（内容 18 字节 + 2 字节填充）");
static_assert(sizeof(struct BPF_TIMEVAL) == 8,
              "LP64 下 BPF 时间戳是 timeval32（8 字节），不是 64 位 timeval");

// =============================================================================
// sysctls snapshotted at feth creation time
//
// These switches are read from sysctl into the interface's private flag bits at the moment of feth_clone_create(),
// and **changing the sysctl after creation has no effect**. They must therefore be verified before creating the feth.
// =============================================================================

/// A feth sysctl that must have a specific value.
///
/// `why` stores a message identifier rather than a ready-made string: this table is constexpr, while the "why"
/// must be rendered in the user's current language, so it can only be deferred to the moment of the error and looked up then.
struct RequiredFethSysctl {
  const char* name;
  std::int32_t required_value;
  Msg why;
};

/// List of sysctls that must be verified before creating a feth.
///
/// If these values are wrong, the frames we read from BPF are not "clean Ethernet frames":
inline constexpr RequiredFethSysctl kRequiredFethSysctls[] = {
    {"net.link.fake.hwcsum", 0, Msg::kNetSysctlWhyHwcsum},
    {"net.link.fake.fcs", 0, Msg::kNetSysctlWhyFcs},
    {"net.link.fake.tso_support", 0, Msg::kNetSysctlWhyTso},
    {"net.link.fake.lro", 0, Msg::kNetSysctlWhyLro},
    {"net.link.fake.trailer_length", 0, Msg::kNetSysctlWhyTrailer},
    {"net.link.fake.separate_frame_header", 0, Msg::kNetSysctlWhySeparateHeader},
};

}  // namespace tetherkitnext::net
