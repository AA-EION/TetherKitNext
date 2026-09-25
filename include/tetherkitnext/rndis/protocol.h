// RNDIS（Remote NDIS）线格式常量与字段布局。
//
// 本文件是全项目 RNDIS 协议常量的**唯一来源**，不允许在别处重复定义。
// 全部数值来自 Microsoft RNDIS 规范，并与 Linux 内核实现交叉验证过
// （drivers/net/usb/rndis_host.c、include/linux/usb/rndis_host.h、
//  drivers/usb/gadget/function/rndis.c、include/linux/usb/cdc.h）。
// 详细的字段表与出处见 docs/RNDIS-PROTOCOL.md。
//
// ★ 三条最容易搞错、必须牢记的规则 ★
//
// 1. **所有多字节字段都是小端**（协议源自 Windows NDIS），一律用 byte_order.h
//    的 LoadLe32 / StoreLe32 访问，禁止用 packed struct 直接映射。
//
// 2. **偏移字段的基准点不是消息起始，而是消息起始 + 8。**
//    PACKET_MSG 的 DataOffset / OOBDataOffset / PerPacketInfoOffset，以及
//    QUERY/SET/QUERY_CMPLT 的 InformationBufferOffset，基准点都是「该组偏移
//    字段所属区块的起始」，即消息起始 + 8 字节处。
//        绝对偏移 = 8 + 字段值
//    所以「数据紧跟 44 字节 PACKET_MSG 头」时 DataOffset = 36，而不是 44。
//    这是实现 RNDIS 时的第一号陷阱。
//
// 3. **INDICATE_STATUS_MSG 的 StatusBufferOffset 是例外**：MS 文档写它的基准点
//    是消息起始（+0），与上面的 +8 约定不一致。这是规范自身的矛盾。实践中该
//    字段绝大多数为 0（MEDIA_CONNECT / MEDIA_DISCONNECT 不带 status buffer），
//    因此解析时必须两种解释都能容忍，且不能因它出错就断开连接。
#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace tetherkitnext::rndis {

// =============================================================================
// 版本与超时
// =============================================================================

/// host 在 INITIALIZE_MSG 中宣称的协议版本。
inline constexpr std::uint32_t kMajorVersion = 1;
inline constexpr std::uint32_t kMinorVersion = 0;

/// 控制通道超时。
///
/// 规范给的 ControlTimeoutPeriod 是 10 秒，但 Linux 因为微软 ActiveSync 实现的
/// 行为把它收缩到 5 秒。这里取 5 秒：设备真要 10 秒才回，链路已经不可用了。
inline constexpr std::uint32_t kControlTimeoutMillis = 5'000;

/// 保活周期。规范的 KeepAliveTimeoutPeriod 是 5 秒，且语义是「距上次从设备
/// 收到任何消息已过 5 秒」才发，而不是无条件每 5 秒发一次。
inline constexpr std::uint32_t kKeepAliveTimeoutMillis = 5'000;

/// 控制消息缓冲区大小。
///
/// 规范要求 host 侧至少 1024 字节（建议 GET_ENCAPSULATED_RESPONSE 的
/// wLength 用 0x400）。Windows 实际用 1025 这个奇怪的值，Linux 照抄。
/// 这里取 1536 留出余量 —— OID_GEN_SUPPORTED_LIST 的返回值可能很长。
inline constexpr std::uint32_t kControlBufferBytes = 1536;

// =============================================================================
// 消息类型码
// =============================================================================

/// 完成消息 = 请求消息 | 此位。
inline constexpr std::uint32_t kMessageCompletionFlag = 0x8000'0000U;

enum class MessageType : std::uint32_t {
  kPacket = 0x0000'0001U,           ///< 数据通道：承载以太帧。
  kInitialize = 0x0000'0002U,       ///< host → device：初始化协商。
  kHalt = 0x0000'0003U,             ///< host → device：终止，**设备不回复**。
  kQuery = 0x0000'0004U,            ///< host → device：读 OID。
  kSet = 0x0000'0005U,              ///< host → device：写 OID。
  kReset = 0x0000'0006U,            ///< host → device：软复位。
  kIndicateStatus = 0x0000'0007U,   ///< device → host：**无 RequestId、无需回复**。
  kKeepAlive = 0x0000'0008U,        ///< 双向都可发起。

  kInitializeComplete = kInitialize | kMessageCompletionFlag,  // 0x80000002
  kQueryComplete = kQuery | kMessageCompletionFlag,            // 0x80000004
  kSetComplete = kSet | kMessageCompletionFlag,                // 0x80000005
  kResetComplete = kReset | kMessageCompletionFlag,            // 0x80000006
  kKeepAliveComplete = kKeepAlive | kMessageCompletionFlag,    // 0x80000008

  /// 私有总线消息，本项目不使用，仅用于识别并忽略。
  kBus = 0xFF00'0001U,
};

/// 面向连接（CONDIS）消息族。802.3 以太网设备**不会**用到，
/// 这里列出仅为了在收到时能明确报告「不支持面向连接设备」而非报「未知消息」。
enum class CondisMessageType : std::uint32_t {
  kCreateVc = 0x0000'8001U,
  kDeleteVc = 0x0000'8002U,
  kActivateVc = 0x0000'8005U,
  kDeactivateVc = 0x0000'8006U,
  kIndicateStatus = 0x0000'8007U,
};

/// 把任意线格式枚举转回它的原始 LE32 数值。
///
/// 约束到「底层类型必须正好是 uint32_t」，这样一旦有人不小心把某个协议枚举的
/// 底层类型改小，这里会直接编译失败，而不是静默写坏线格式。
template <typename E>
  requires std::is_enum_v<E> && std::same_as<std::underlying_type_t<E>, std::uint32_t>
[[nodiscard]] constexpr std::uint32_t ToRaw(E value) noexcept {
  return static_cast<std::uint32_t>(value);
}

/// 是否为完成类消息。
[[nodiscard]] constexpr bool IsCompletion(std::uint32_t message_type) noexcept {
  return (message_type & kMessageCompletionFlag) != 0;
}

/// 是否属于 CONDIS（面向连接）消息族。
[[nodiscard]] constexpr bool IsCondis(std::uint32_t message_type) noexcept {
  return (message_type & ~kMessageCompletionFlag) >= 0x0000'8000U &&
         (message_type & ~kMessageCompletionFlag) <= 0x0000'8FFFU;
}

/// 消息类型的可读名字；未知类型返回空。
[[nodiscard]] std::string_view MessageTypeName(std::uint32_t message_type) noexcept;

// =============================================================================
// 消息长度与字段偏移
//
// 所有字段都是 LE32。下面的 kXxxOffset 常量是**消息内的字节偏移**，
// 供 LoadLe32(buffer + kXxxOffset) 直接使用。
// =============================================================================

/// 每条 RNDIS 消息共有的前两个字段。
inline constexpr std::uint32_t kMessageTypeOffset = 0;
inline constexpr std::uint32_t kMessageLengthOffset = 4;

/// 最短的合法 RNDIS 消息就是 Type + Length。
inline constexpr std::uint32_t kMessageHeaderBytes = 8;

/// 「偏移字段」的基准点：消息起始 + 8。见文件头第 2 条规则。
inline constexpr std::uint32_t kOffsetFieldBase = 8;

// ---- REMOTE_NDIS_INITIALIZE_MSG（24 字节）----
inline constexpr std::uint32_t kInitializeMsgBytes = 24;
inline constexpr std::uint32_t kInitializeRequestIdOffset = 8;
inline constexpr std::uint32_t kInitializeMajorVersionOffset = 12;
inline constexpr std::uint32_t kInitializeMinorVersionOffset = 16;
inline constexpr std::uint32_t kInitializeMaxTransferSizeOffset = 20;

// ---- REMOTE_NDIS_INITIALIZE_CMPLT（52 字节）----
inline constexpr std::uint32_t kInitializeCmpltBytes = 52;
inline constexpr std::uint32_t kInitializeCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kInitializeCmpltStatusOffset = 12;
inline constexpr std::uint32_t kInitializeCmpltMajorVersionOffset = 16;
inline constexpr std::uint32_t kInitializeCmpltMinorVersionOffset = 20;
inline constexpr std::uint32_t kInitializeCmpltDeviceFlagsOffset = 24;
inline constexpr std::uint32_t kInitializeCmpltMediumOffset = 28;
inline constexpr std::uint32_t kInitializeCmpltMaxPacketsPerMessageOffset = 32;
inline constexpr std::uint32_t kInitializeCmpltMaxTransferSizeOffset = 36;
inline constexpr std::uint32_t kInitializeCmpltPacketAlignmentFactorOffset = 40;
inline constexpr std::uint32_t kInitializeCmpltAfListOffsetOffset = 44;
inline constexpr std::uint32_t kInitializeCmpltAfListSizeOffset = 48;

// ---- REMOTE_NDIS_HALT_MSG（12 字节，设备不回复）----
inline constexpr std::uint32_t kHaltMsgBytes = 12;
inline constexpr std::uint32_t kHaltRequestIdOffset = 8;

// ---- REMOTE_NDIS_QUERY_MSG / SET_MSG（头部 28 字节）----
inline constexpr std::uint32_t kQuerySetHeaderBytes = 28;
inline constexpr std::uint32_t kQuerySetRequestIdOffset = 8;
inline constexpr std::uint32_t kQuerySetOidOffset = 12;
inline constexpr std::uint32_t kQuerySetInfoBufferLengthOffset = 16;
inline constexpr std::uint32_t kQuerySetInfoBufferOffsetOffset = 20;
inline constexpr std::uint32_t kQuerySetDeviceVcHandleOffset = 24;

/// 「信息缓冲区紧跟 28 字节头部」时 InformationBufferOffset 应填的值。
inline constexpr std::uint32_t kQuerySetInlineInfoOffset = kQuerySetHeaderBytes - kOffsetFieldBase;
static_assert(kQuerySetInlineInfoOffset == 20);

// ---- REMOTE_NDIS_QUERY_CMPLT（头部 24 字节）----
inline constexpr std::uint32_t kQueryCmpltHeaderBytes = 24;
inline constexpr std::uint32_t kQueryCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kQueryCmpltStatusOffset = 12;
inline constexpr std::uint32_t kQueryCmpltInfoBufferLengthOffset = 16;
inline constexpr std::uint32_t kQueryCmpltInfoBufferOffsetOffset = 20;

/// 「信息缓冲区紧跟 24 字节头部」时 InformationBufferOffset 的值。
inline constexpr std::uint32_t kQueryCmpltInlineInfoOffset = kQueryCmpltHeaderBytes - kOffsetFieldBase;
static_assert(kQueryCmpltInlineInfoOffset == 16);

// ---- REMOTE_NDIS_SET_CMPLT（16 字节）----
inline constexpr std::uint32_t kSetCmpltBytes = 16;
inline constexpr std::uint32_t kSetCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kSetCmpltStatusOffset = 12;

// ---- REMOTE_NDIS_RESET_MSG（12 字节，**没有 RequestId**）----
inline constexpr std::uint32_t kResetMsgBytes = 12;
/// offset 8 是 Reserved，而非 RequestId。因此 RESET 无法用 ID 配对，
/// 同一时刻只能有一个 RESET 在飞。
inline constexpr std::uint32_t kResetReservedOffset = 8;

// ---- REMOTE_NDIS_RESET_CMPLT（16 字节，**没有 RequestId**）----
inline constexpr std::uint32_t kResetCmpltBytes = 16;
inline constexpr std::uint32_t kResetCmpltStatusOffset = 8;
/// 非零表示寻址信息（packet filter、组播表、functional address）在复位中丢失，
/// host 必须重发相应的 SET。
inline constexpr std::uint32_t kResetCmpltAddressingResetOffset = 12;

// ---- REMOTE_NDIS_INDICATE_STATUS_MSG（头部 20 字节，**没有 RequestId**）----
inline constexpr std::uint32_t kIndicateStatusHeaderBytes = 20;
inline constexpr std::uint32_t kIndicateStatusStatusOffset = 8;
inline constexpr std::uint32_t kIndicateStatusBufferLengthOffset = 12;
/// 注意基准点争议，见文件头第 3 条规则。
inline constexpr std::uint32_t kIndicateStatusBufferOffsetOffset = 16;

/// INDICATE_STATUS 携带的诊断信息结构（RNDIS_Diagnostic_Info，8 字节）。
inline constexpr std::uint32_t kDiagnosticInfoBytes = 8;
inline constexpr std::uint32_t kDiagnosticInfoDiagStatusOffset = 0;
inline constexpr std::uint32_t kDiagnosticInfoErrorOffsetOffset = 4;

// ---- REMOTE_NDIS_KEEPALIVE_MSG（12 字节）/ KEEPALIVE_CMPLT（16 字节）----
inline constexpr std::uint32_t kKeepAliveMsgBytes = 12;
inline constexpr std::uint32_t kKeepAliveRequestIdOffset = 8;
inline constexpr std::uint32_t kKeepAliveCmpltBytes = 16;
inline constexpr std::uint32_t kKeepAliveCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kKeepAliveCmpltStatusOffset = 12;

// ---- REMOTE_NDIS_PACKET_MSG（头部 44 字节）----
inline constexpr std::uint32_t kPacketMsgHeaderBytes = 44;
inline constexpr std::uint32_t kPacketDataOffsetOffset = 8;
inline constexpr std::uint32_t kPacketDataLengthOffset = 12;
inline constexpr std::uint32_t kPacketOobDataOffsetOffset = 16;
inline constexpr std::uint32_t kPacketOobDataLengthOffset = 20;
inline constexpr std::uint32_t kPacketNumOobDataElementsOffset = 24;
inline constexpr std::uint32_t kPacketPerPacketInfoOffsetOffset = 28;
inline constexpr std::uint32_t kPacketPerPacketInfoLengthOffset = 32;
inline constexpr std::uint32_t kPacketVcHandleOffset = 36;
inline constexpr std::uint32_t kPacketReservedOffset = 40;

/// 「以太帧紧跟 44 字节头部」时 DataOffset 应填的值。**是 36，不是 44。**
inline constexpr std::uint32_t kPacketInlineDataOffset = kPacketMsgHeaderBytes - kOffsetFieldBase;
static_assert(kPacketInlineDataOffset == 36, "DataOffset 基准点是消息起始 +8，见文件头规则 2");

/// device → host 方向，多包聚合时每个 PACKET_MSG 应从 8 字节整数倍偏移开始。
inline constexpr std::uint32_t kDeviceToHostPacketAlignment = 8;

/// PacketAlignmentFactor 的合法上界（1 << 7 = 128 字节）。
/// 设备报出更大的值属于协议违规，实现里必须钳位，否则会算出荒谬的填充长度。
inline constexpr std::uint32_t kMaxPacketAlignmentFactor = 7;

// =============================================================================
// 状态码
// =============================================================================

/// RNDIS_STATUS_* 状态码。
///
/// 刻意命名为 StatusCode 而非 Status：`tetherkitnext::Status` 已经是
/// `std::expected<void, Error>` 的别名，在 tetherkitnext::rndis 里再定义一个
/// `Status` 会遮蔽它，导致本命名空间内无法书写「可能失败但无返回值」的签名。
enum class StatusCode : std::uint32_t {
  // ---- 成功 / 挂起 ----
  kSuccess = 0x0000'0000U,
  kPending = 0x0000'0103U,

  // ---- 信息类 ----
  kNotRecognized = 0x0001'0001U,
  kNotCopied = 0x0001'0002U,
  kNotAccepted = 0x0001'0003U,
  kCallActive = 0x0001'0007U,

  // ---- 指示类（0x4001xxxx，出现在 INDICATE_STATUS_MSG 里）----
  kOnline = 0x4001'0003U,
  kResetStart = 0x4001'0004U,
  kResetEnd = 0x4001'0005U,
  kRingStatus = 0x4001'0006U,
  kClosed = 0x4001'0007U,
  kWanLineUp = 0x4001'0008U,
  kWanLineDown = 0x4001'0009U,
  kWanFragment = 0x4001'000AU,
  kMediaConnect = 0x4001'000BU,     ///< 链路 up —— 我们据此把 feth 置 UP。
  kMediaDisconnect = 0x4001'000CU,  ///< 链路 down。
  kHardwareLineUp = 0x4001'000DU,
  kHardwareLineDown = 0x4001'000EU,
  kInterfaceUp = 0x4001'000FU,
  kInterfaceDown = 0x4001'0010U,
  kMediaBusy = 0x4001'0011U,
  kMediaSpecificIndication = 0x4001'0012U,
  kLinkSpeedChange = 0x4001'0013U,
  kNetworkChange = 0x4001'0018U,

  // ---- 警告类（0x8xxxxxxx）----
  kBufferOverflow = 0x8000'0005U,
  kNotResettable = 0x8001'0001U,
  kSoftErrors = 0x8001'0003U,
  kHardErrors = 0x8001'0004U,

  // ---- 错误类（0xCxxxxxxx）----
  kFailure = 0xC000'0001U,
  kResources = 0xC000'009AU,
  kNotSupported = 0xC000'00BBU,
  kClosing = 0xC001'0002U,
  kBadVersion = 0xC001'0004U,
  kBadCharacteristics = 0xC001'0005U,
  kAdapterNotFound = 0xC001'0006U,
  kOpenFailed = 0xC001'0007U,
  kDeviceFailed = 0xC001'0008U,
  kMulticastFull = 0xC001'0009U,
  kMulticastExists = 0xC001'000AU,
  kMulticastNotFound = 0xC001'000BU,
  kRequestAborted = 0xC001'000CU,
  kResetInProgress = 0xC001'000DU,
  kClosingIndicating = 0xC001'000EU,
  kInvalidPacket = 0xC001'000FU,
  kOpenListFull = 0xC001'0010U,
  kAdapterNotReady = 0xC001'0011U,
  kAdapterNotOpen = 0xC001'0012U,
  kNotIndicating = 0xC001'0013U,
  kInvalidLength = 0xC001'0014U,
  kInvalidData = 0xC001'0015U,
  kBufferTooShort = 0xC001'0016U,
  kInvalidOid = 0xC001'0017U,
  kAdapterRemoved = 0xC001'0018U,
  kUnsupportedMedia = 0xC001'0019U,
  kGroupAddressInUse = 0xC001'001AU,
  kNoCable = 0xC001'001FU,
  kTokenRingOpenError = 0xC001'1000U,
};

/// 是否为失败状态（0xC0000000 及以上按 NDIS 约定是错误）。
[[nodiscard]] constexpr bool IsFailure(std::uint32_t status) noexcept {
  return (status & 0xC000'0000U) == 0xC000'0000U;
}

/// 状态码的可读名字；未知值返回空。
[[nodiscard]] std::string_view StatusName(std::uint32_t status) noexcept;

// =============================================================================
// OID
// =============================================================================

enum class Oid : std::uint32_t {
  // ---- 通用（OID_GEN_*）----
  kGenSupportedList = 0x0001'0101U,      ///< 变长：N 个 LE32 OID。查询时 in_len 必须为 0。
  kGenHardwareStatus = 0x0001'0102U,     ///< LE32：0=Ready 1=Initializing 2=Reset 3=Closing 4=NotReady
  kGenMediaSupported = 0x0001'0103U,     ///< LE32 数组。
  kGenMediaInUse = 0x0001'0104U,         ///< LE32 数组。
  kGenMaximumFrameSize = 0x0001'0106U,   ///< LE32：**不含** 14 字节以太头，典型 1500。
  kGenLinkSpeed = 0x0001'0107U,          ///< LE32：单位 **100 bps**；断连时为 0。
  kGenTransmitBlockSize = 0x0001'010AU,  ///< LE32
  kGenReceiveBlockSize = 0x0001'010BU,   ///< LE32
  kGenVendorId = 0x0001'010CU,           ///< LE32：低 24 位是 IEEE OUI。
  kGenVendorDescription = 0x0001'010DU,  ///< 变长 ASCII，**不保证 NUL 结尾**。
  kGenCurrentPacketFilter = 0x0001'010EU,  ///< LE32 位掩码，可读可写。
  kGenMaximumTotalSize = 0x0001'0111U,   ///< LE32：含头部的最大总帧长。
  kGenMacOptions = 0x0001'0113U,         ///< LE32：MacOption 位掩码。
  kGenMediaConnectStatus = 0x0001'0114U,  ///< LE32：**0=已连接，1=断开**（注意 0 是连接）。
  kGenPhysicalMedium = 0x0001'0202U,     ///< LE32：PhysicalMedium；**可选 OID**，失败不致命。

  // ---- 统计（OID_GEN_*_OK/ERROR）：可能返回 4 或 8 字节，两者都要接受 ----
  kGenXmitOk = 0x0002'0101U,
  kGenRcvOk = 0x0002'0102U,
  kGenXmitError = 0x0002'0103U,
  kGenRcvError = 0x0002'0104U,
  kGenRcvNoBuffer = 0x0002'0105U,

  // ---- 802.3 专用 ----
  kEthernetPermanentAddress = 0x0101'0101U,  ///< 6 字节 MAC。
  kEthernetCurrentAddress = 0x0101'0102U,    ///< 6 字节 MAC。
  kEthernetMulticastList = 0x0101'0103U,     ///< SET：6N 字节 MAC 数组。**不要依赖 QUERY**。
  kEthernetMaximumListSize = 0x0101'0104U,   ///< LE32：组播表最大条数。
  kEthernetMacOptions = 0x0101'0105U,        ///< LE32：Ethernet802_3MacOption 位掩码。
};

/// OID 的可读名字；未知值返回空。
[[nodiscard]] std::string_view OidName(std::uint32_t oid) noexcept;

/// 该 OID 的返回值是否为变长。
///
/// 这个区分很关键：对**定长** OID，QUERY 的 InformationBufferLength 必须设成
/// ≥ 期望响应长度并在消息尾附上同样多的零字节，否则微软 ActiveSync 实现会回
/// RNDIS_STATUS_INVALID_LENGTH；而对**变长** OID 必须传 0，否则同样出错。
/// 这条是嗅探 ActiveSync 4.1 的 Windows 驱动才发现的未文档化行为。
[[nodiscard]] constexpr bool IsVariableLengthOid(Oid oid) noexcept {
  switch (oid) {
    case Oid::kGenSupportedList:
    case Oid::kGenVendorDescription:
    case Oid::kGenMediaSupported:
    case Oid::kGenMediaInUse:
    case Oid::kEthernetMulticastList:
      return true;
    default:
      return false;
  }
}

// =============================================================================
// 包过滤位掩码（NDIS_PACKET_TYPE_*）
// =============================================================================

enum class PacketFilter : std::uint32_t {
  kDirected = 0x0000'0001U,       ///< 目的 MAC 等于本机地址。
  kMulticast = 0x0000'0002U,      ///< 在组播表中的组播地址。
  kAllMulticast = 0x0000'0004U,   ///< 全部组播，不看组播表。
  kBroadcast = 0x0000'0008U,      ///< ff:ff:ff:ff:ff:ff
  kSourceRouting = 0x0000'0010U,
  kPromiscuous = 0x0000'0020U,    ///< 全部帧。
  kSmt = 0x0000'0040U,
  kAllLocal = 0x0000'0080U,
  kGroup = 0x0000'1000U,
  kAllFunctional = 0x0000'2000U,
  kFunctional = 0x0000'4000U,
  kMacFrame = 0x0000'8000U,
};

/// 本项目使用的包过滤组合，与 Linux rndis_host 保持一致（0x0000002D）。
///
/// 为什么要开 PROMISCUOUS：我们把设备当成一根「网线」桥接到 feth，
/// 主机侧可能收发任意 MAC 的帧（例如上层再做桥接或虚拟机），不能只收
/// directed。ALL_MULTICAST 同理 —— 避免维护组播表还漏掉 IPv6 邻居发现。
inline constexpr std::uint32_t kDefaultPacketFilter =
    ToRaw(PacketFilter::kDirected) | ToRaw(PacketFilter::kBroadcast) |
    ToRaw(PacketFilter::kAllMulticast) | ToRaw(PacketFilter::kPromiscuous);
static_assert(kDefaultPacketFilter == 0x0000'002DU);

// =============================================================================
// 介质、物理介质、设备标志
// =============================================================================

enum class Medium : std::uint32_t {
  kEthernet = 0x0000'0000U,  ///< 802.3；同时也是 UNSPECIFIED。本项目只支持这一种。
  kTokenRing = 1,           ///< 802.5
  kFddi = 2,
  kWan = 3,
  kLocalTalk = 4,
  kArcnetRaw = 6,
  kArcnet8782 = 7,  ///< ARCNET 878.2
  kAtm = 8,
  kWirelessLan = 9,
  kIrda = 0x0A,
  kBpc = 0x0B,
  kCoWan = 0x0C,
  kIeee1394 = 0x0D,
};

enum class PhysicalMedium : std::uint32_t {
  kUnspecified = 0,
  kWirelessLan = 1,
  kCableModem = 2,
  kPhoneLine = 3,
  kPowerLine = 4,
  kDsl = 5,
  kFibreChannel = 6,
  kIeee1394 = 7,
  kWirelessWan = 8,
};

/// INITIALIZE_CMPLT 的 DeviceFlags 位掩码。
enum class DeviceFlags : std::uint32_t {
  kConnectionless = 0x0000'0001U,      ///< 以太网设备应置此位。
  kConnectionOriented = 0x0000'0002U,  ///< CONDIS，本项目不支持。
  kRawData = 0x0000'0004U,
};

/// OID_GEN_MEDIA_CONNECT_STATUS 的取值。**注意 0 表示已连接。**
enum class MediaState : std::uint32_t {
  kConnected = 0,
  kDisconnected = 1,
};

/// OID_GEN_MAC_OPTIONS 位掩码。
enum class MacOption : std::uint32_t {
  kCopyLookaheadData = 0x0000'0001U,
  kReceiveSerialized = 0x0000'0002U,
  kTransfersNotPend = 0x0000'0004U,
  kNoLoopback = 0x0000'0008U,
  kFullDuplex = 0x0000'0010U,
  kEotxIndication = 0x0000'0020U,
  k8021pPriority = 0x0000'0040U,
};

// =============================================================================
// USB 层常量（控制通道与描述符识别）
// =============================================================================

/// SEND_ENCAPSULATED_COMMAND 的 bRequest（CDC 规范）。
inline constexpr std::uint8_t kRequestSendEncapsulatedCommand = 0x00;
/// GET_ENCAPSULATED_RESPONSE 的 bRequest。
inline constexpr std::uint8_t kRequestGetEncapsulatedResponse = 0x01;

/// bmRequestType：主机 → 设备，类请求，接口收件人。
inline constexpr std::uint8_t kControlOutRequestType = 0x21;
/// bmRequestType：设备 → 主机，类请求，接口收件人。
inline constexpr std::uint8_t kControlInRequestType = 0xA1;

/// 中断 IN 端点上的通知：8 字节 = 两个 LE32。
///
/// 注意这**不是** CDC 的 usb_cdc_notification 结构，而是 RNDIS 自有格式：
///   offset 0..3 = Notification（0x00000001 = RESPONSE_AVAILABLE）
///   offset 4..7 = Reserved（0）
inline constexpr std::uint32_t kNotificationBytes = 8;
inline constexpr std::uint32_t kNotificationResponseAvailable = 0x0000'0001U;
inline constexpr std::uint32_t kNotificationValueOffset = 0;
inline constexpr std::uint32_t kNotificationReservedOffset = 4;

/// USB 接口 class/subclass/protocol 三元组，用于识别 RNDIS 设备。
struct InterfaceSignature {
  std::uint8_t interface_class;
  std::uint8_t interface_subclass;
  std::uint8_t interface_protocol;

  [[nodiscard]] constexpr bool operator==(const InterfaceSignature&) const = default;
};

/// RNDIS 通信类接口的全部已知形态（实测在用的四种）。
///
/// (a) 标准 MS RNDIS：CDC Communication / ACM / vendor-specific protocol
/// (b) ActiveSync（Windows Mobile 5 / Windows Phone）：USB_CLASS_MISC
/// (c) 无线 RNDIS（手机 USB 网络共享、WWAN 模块）：Wireless Controller / RF / RNDIS
/// (d) Novatel/Verizon USB730L 变体
inline constexpr InterfaceSignature kControlSignatureMicrosoft{0x02, 0x02, 0xFF};
inline constexpr InterfaceSignature kControlSignatureActiveSync{0xEF, 0x01, 0x01};
inline constexpr InterfaceSignature kControlSignatureWireless{0xE0, 0x01, 0x03};
inline constexpr InterfaceSignature kControlSignatureNovatel{0xEF, 0x04, 0x01};

/// RNDIS 数据类接口：CDC Data，无子类无协议。
inline constexpr InterfaceSignature kDataSignature{0x0A, 0x00, 0x00};

/// 是否为已知的 RNDIS 通信类接口签名。
[[nodiscard]] constexpr bool IsRndisControlSignature(const InterfaceSignature& signature) noexcept {
  return signature == kControlSignatureMicrosoft || signature == kControlSignatureActiveSync ||
         signature == kControlSignatureWireless || signature == kControlSignatureNovatel;
}

// =============================================================================
// 帧尺寸推导
// =============================================================================

/// 以太头长度。
inline constexpr std::uint32_t kEthernetHeaderBytes = 14;

/// RNDIS 数据通道的「硬头部」= 以太头 + PACKET_MSG 头。
///
/// Linux 的 hard_header_len 就是这个 58，MaxTransferSize 的推导以它为基准。
inline constexpr std::uint32_t kHardHeaderBytes = kEthernetHeaderBytes + kPacketMsgHeaderBytes;
static_assert(kHardHeaderBytes == 58);

/// 以太网标准 MTU。
inline constexpr std::uint32_t kDefaultMtu = 1500;

/// 给定 MTU，计算一帧在 USB 上占用的最大字节数（含 RNDIS 头与以太头）。
[[nodiscard]] constexpr std::uint32_t HardMtuFor(std::uint32_t mtu) noexcept {
  return mtu + kHardHeaderBytes;
}

/// 计算 INITIALIZE_MSG 里该宣称的 MaxTransferSize。
///
/// 沿用 Linux 的算法：`(hard_mtu + maxpacket + 1) & ~(maxpacket - 1)`，
/// 即向上对齐到端点最大包长的整数倍再留一个包的余量。
/// 高速（maxpacket=512）下 MTU=1500 得 2048；全速（64）下得 1600。
[[nodiscard]] constexpr std::uint32_t MaxTransferSizeFor(std::uint32_t mtu,
                                                         std::uint32_t endpoint_max_packet) noexcept {
  const std::uint32_t hard_mtu = HardMtuFor(mtu);
  return (hard_mtu + endpoint_max_packet + 1) & ~(endpoint_max_packet - 1);
}

static_assert(MaxTransferSizeFor(1500, 512) == 2048);
static_assert(MaxTransferSizeFor(1500, 64) == 1600);

/// USB 1.1 设备的 MaxTransferSize 硬上限（规范要求）。
inline constexpr std::uint32_t kUsb11MaxTransferSize = 0x4000;

}  // namespace tetherkitnext::rndis
