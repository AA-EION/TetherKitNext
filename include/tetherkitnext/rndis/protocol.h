// RNDIS (Remote NDIS) wire-format constants and field layouts.
//
// This file is the **sole source** of RNDIS protocol constants for the whole project; duplicating definitions elsewhere is not allowed.
// All values come from the Microsoft RNDIS specification and have been cross-validated against the Linux kernel implementation
// (drivers/net/usb/rndis_host.c, include/linux/usb/rndis_host.h,
//  drivers/usb/gadget/function/rndis.c, include/linux/usb/cdc.h).
// See docs/RNDIS-PROTOCOL.md for detailed field tables and sources.
//
// * Three rules that are the easiest to get wrong and must be remembered *
//
// 1. **All multi-byte fields are little-endian** (the protocol originates from Windows NDIS); always access them with byte_order.h's
//    LoadLe32 / StoreLe32, and do not map them directly with packed structs.
//
// 2. **The base point of offset fields is not the message start, but message start + 8.**
//    PACKET_MSG's DataOffset / OOBDataOffset / PerPacketInfoOffset, and
//    QUERY/SET/QUERY_CMPLT's InformationBufferOffset all have the base point "the start of the block
//    that this group of offset fields belongs to", i.e. at message start + 8 bytes.
//        absolute offset = 8 + field value
//    So when "the data immediately follows the 44-byte PACKET_MSG header", DataOffset = 36, not 44.
//    This is the number one trap when implementing RNDIS.
//
// 3. **StatusBufferOffset of INDICATE_STATUS_MSG is the exception**: the MS documentation says its base point
//    is the message start (+0), inconsistent with the +8 convention above. This is a contradiction in the spec itself. In practice this
//    field is mostly 0 (MEDIA_CONNECT / MEDIA_DISCONNECT carry no status buffer),
//    so parsing must tolerate both interpretations, and must not drop the connection because of an error in it.
#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace tetherkitnext::rndis {

// =============================================================================
// Version and timeouts
// =============================================================================

/// The protocol version the host claims in INITIALIZE_MSG.
inline constexpr std::uint32_t kMajorVersion = 1;
inline constexpr std::uint32_t kMinorVersion = 0;

/// Control channel timeout.
///
/// The spec gives a ControlTimeoutPeriod of 10 seconds, but Linux shrank it to 5 seconds because of the behavior of Microsoft's
/// ActiveSync implementation. 5 seconds is used here: if the device really needs 10 seconds to reply, the link is already unusable.
inline constexpr std::uint32_t kControlTimeoutMillis = 5'000;

/// Keepalive period. The spec's KeepAliveTimeoutPeriod is 5 seconds, and the semantics are to send only when "5 seconds have passed since
/// any message was last received from the device", not to send unconditionally every 5 seconds.
inline constexpr std::uint32_t kKeepAliveTimeoutMillis = 5'000;

/// Control message buffer size.
///
/// The spec requires at least 1024 bytes on the host side (GET_ENCAPSULATED_RESPONSE's
/// wLength is recommended to be 0x400). Windows actually uses the odd value 1025, and Linux copied it.
/// 1536 is used here to leave margin -- the return value of OID_GEN_SUPPORTED_LIST can be very long.
inline constexpr std::uint32_t kControlBufferBytes = 1536;

// =============================================================================
// Message type codes
// =============================================================================

/// Completion message = request message | this bit.
inline constexpr std::uint32_t kMessageCompletionFlag = 0x8000'0000U;

enum class MessageType : std::uint32_t {
  kPacket = 0x0000'0001U,           ///< Data channel: carries Ethernet frames.
  kInitialize = 0x0000'0002U,       ///< host -> device: initialization negotiation.
  kHalt = 0x0000'0003U,             ///< host -> device: terminate, **the device does not reply**.
  kQuery = 0x0000'0004U,            ///< host -> device: read an OID.
  kSet = 0x0000'0005U,              ///< host -> device: write an OID.
  kReset = 0x0000'0006U,            ///< host -> device: soft reset.
  kIndicateStatus = 0x0000'0007U,   ///< device -> host: **no RequestId, no reply needed**.
  kKeepAlive = 0x0000'0008U,        ///< Can be initiated by either side.

  kInitializeComplete = kInitialize | kMessageCompletionFlag,  // 0x80000002
  kQueryComplete = kQuery | kMessageCompletionFlag,            // 0x80000004
  kSetComplete = kSet | kMessageCompletionFlag,                // 0x80000005
  kResetComplete = kReset | kMessageCompletionFlag,            // 0x80000006
  kKeepAliveComplete = kKeepAlive | kMessageCompletionFlag,    // 0x80000008

  /// Private bus message, not used by this project; only used to recognize and ignore it.
  kBus = 0xFF00'0001U,
};

/// Connection-oriented (CONDIS) message family. 802.3 Ethernet devices **never** use it;
/// it is listed here only so that on receipt we can explicitly report "connection-oriented devices are not supported" rather than "unknown message".
enum class CondisMessageType : std::uint32_t {
  kCreateVc = 0x0000'8001U,
  kDeleteVc = 0x0000'8002U,
  kActivateVc = 0x0000'8005U,
  kDeactivateVc = 0x0000'8006U,
  kIndicateStatus = 0x0000'8007U,
};

/// Converts any wire-format enum back to its raw LE32 value.
///
/// Constrained to "the underlying type must be exactly uint32_t", so that if someone accidentally shrinks the
/// underlying type of some protocol enum, this fails to compile directly instead of silently corrupting the wire format.
template <typename E>
  requires std::is_enum_v<E> && std::same_as<std::underlying_type_t<E>, std::uint32_t>
[[nodiscard]] constexpr std::uint32_t ToRaw(E value) noexcept {
  return static_cast<std::uint32_t>(value);
}

/// Whether it is a completion-type message.
[[nodiscard]] constexpr bool IsCompletion(std::uint32_t message_type) noexcept {
  return (message_type & kMessageCompletionFlag) != 0;
}

/// Whether it belongs to the CONDIS (connection-oriented) message family.
[[nodiscard]] constexpr bool IsCondis(std::uint32_t message_type) noexcept {
  return (message_type & ~kMessageCompletionFlag) >= 0x0000'8000U &&
         (message_type & ~kMessageCompletionFlag) <= 0x0000'8FFFU;
}

/// Readable name of a message type; an unknown type returns empty.
[[nodiscard]] std::string_view MessageTypeName(std::uint32_t message_type) noexcept;

// =============================================================================
// Message lengths and field offsets
//
// All fields are LE32. The kXxxOffset constants below are **byte offsets within the message**,
// for direct use by LoadLe32(buffer + kXxxOffset).
// =============================================================================

/// The first two fields common to every RNDIS message.
inline constexpr std::uint32_t kMessageTypeOffset = 0;
inline constexpr std::uint32_t kMessageLengthOffset = 4;

/// The shortest legal RNDIS message is just Type + Length.
inline constexpr std::uint32_t kMessageHeaderBytes = 8;

/// The base point of "offset fields": message start + 8. See rule 2 in the file header.
inline constexpr std::uint32_t kOffsetFieldBase = 8;

// ---- REMOTE_NDIS_INITIALIZE_MSG (24 bytes) ----
inline constexpr std::uint32_t kInitializeMsgBytes = 24;
inline constexpr std::uint32_t kInitializeRequestIdOffset = 8;
inline constexpr std::uint32_t kInitializeMajorVersionOffset = 12;
inline constexpr std::uint32_t kInitializeMinorVersionOffset = 16;
inline constexpr std::uint32_t kInitializeMaxTransferSizeOffset = 20;

// ---- REMOTE_NDIS_INITIALIZE_CMPLT (52 bytes) ----
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

// ---- REMOTE_NDIS_HALT_MSG (12 bytes, the device does not reply) ----
inline constexpr std::uint32_t kHaltMsgBytes = 12;
inline constexpr std::uint32_t kHaltRequestIdOffset = 8;

// ---- REMOTE_NDIS_QUERY_MSG / SET_MSG (28-byte header) ----
inline constexpr std::uint32_t kQuerySetHeaderBytes = 28;
inline constexpr std::uint32_t kQuerySetRequestIdOffset = 8;
inline constexpr std::uint32_t kQuerySetOidOffset = 12;
inline constexpr std::uint32_t kQuerySetInfoBufferLengthOffset = 16;
inline constexpr std::uint32_t kQuerySetInfoBufferOffsetOffset = 20;
inline constexpr std::uint32_t kQuerySetDeviceVcHandleOffset = 24;

/// The value InformationBufferOffset should be filled with when "the information buffer immediately follows the 28-byte header".
inline constexpr std::uint32_t kQuerySetInlineInfoOffset = kQuerySetHeaderBytes - kOffsetFieldBase;
static_assert(kQuerySetInlineInfoOffset == 20);

// ---- REMOTE_NDIS_QUERY_CMPLT (24-byte header) ----
inline constexpr std::uint32_t kQueryCmpltHeaderBytes = 24;
inline constexpr std::uint32_t kQueryCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kQueryCmpltStatusOffset = 12;
inline constexpr std::uint32_t kQueryCmpltInfoBufferLengthOffset = 16;
inline constexpr std::uint32_t kQueryCmpltInfoBufferOffsetOffset = 20;

/// The value of InformationBufferOffset when "the information buffer immediately follows the 24-byte header".
inline constexpr std::uint32_t kQueryCmpltInlineInfoOffset = kQueryCmpltHeaderBytes - kOffsetFieldBase;
static_assert(kQueryCmpltInlineInfoOffset == 16);

// ---- REMOTE_NDIS_SET_CMPLT (16 bytes) ----
inline constexpr std::uint32_t kSetCmpltBytes = 16;
inline constexpr std::uint32_t kSetCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kSetCmpltStatusOffset = 12;

// ---- REMOTE_NDIS_RESET_MSG (12 bytes, **no RequestId**) ----
inline constexpr std::uint32_t kResetMsgBytes = 12;
/// Offset 8 is Reserved, not RequestId. So RESET cannot be paired by ID,
/// and only one RESET can be in flight at a time.
inline constexpr std::uint32_t kResetReservedOffset = 8;

// ---- REMOTE_NDIS_RESET_CMPLT (16 bytes, **no RequestId**) ----
inline constexpr std::uint32_t kResetCmpltBytes = 16;
inline constexpr std::uint32_t kResetCmpltStatusOffset = 8;
/// Non-zero means addressing information (packet filter, multicast table, functional address) was lost in the reset,
/// and the host must resend the corresponding SETs.
inline constexpr std::uint32_t kResetCmpltAddressingResetOffset = 12;

// ---- REMOTE_NDIS_INDICATE_STATUS_MSG (20-byte header, **no RequestId**) ----
inline constexpr std::uint32_t kIndicateStatusHeaderBytes = 20;
inline constexpr std::uint32_t kIndicateStatusStatusOffset = 8;
inline constexpr std::uint32_t kIndicateStatusBufferLengthOffset = 12;
/// Note the base point dispute; see rule 3 in the file header.
inline constexpr std::uint32_t kIndicateStatusBufferOffsetOffset = 16;

/// The diagnostic information structure carried by INDICATE_STATUS (RNDIS_Diagnostic_Info, 8 bytes).
inline constexpr std::uint32_t kDiagnosticInfoBytes = 8;
inline constexpr std::uint32_t kDiagnosticInfoDiagStatusOffset = 0;
inline constexpr std::uint32_t kDiagnosticInfoErrorOffsetOffset = 4;

// ---- REMOTE_NDIS_KEEPALIVE_MSG (12 bytes) / KEEPALIVE_CMPLT (16 bytes) ----
inline constexpr std::uint32_t kKeepAliveMsgBytes = 12;
inline constexpr std::uint32_t kKeepAliveRequestIdOffset = 8;
inline constexpr std::uint32_t kKeepAliveCmpltBytes = 16;
inline constexpr std::uint32_t kKeepAliveCmpltRequestIdOffset = 8;
inline constexpr std::uint32_t kKeepAliveCmpltStatusOffset = 12;

// ---- REMOTE_NDIS_PACKET_MSG (44-byte header) ----
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

/// The value DataOffset should be filled with when "the Ethernet frame immediately follows the 44-byte header". **It is 36, not 44.**
inline constexpr std::uint32_t kPacketInlineDataOffset = kPacketMsgHeaderBytes - kOffsetFieldBase;
static_assert(kPacketInlineDataOffset == 36, "DataOffset 基准点是消息起始 +8，见文件头规则 2");

/// In the device -> host direction, when multiple packets are aggregated, each PACKET_MSG should start at an offset that is a multiple of 8 bytes.
inline constexpr std::uint32_t kDeviceToHostPacketAlignment = 8;

/// Legal upper bound of PacketAlignmentFactor (1 << 7 = 128 bytes).
/// A device reporting a larger value is a protocol violation; the implementation must clamp it, otherwise an absurd padding length would be computed.
inline constexpr std::uint32_t kMaxPacketAlignmentFactor = 7;

// =============================================================================
// Status codes
// =============================================================================

/// RNDIS_STATUS_* status codes.
///
/// Deliberately named StatusCode rather than Status: `tetherkitnext::Status` is already an alias of
/// `std::expected<void, Error>`, and defining another `Status` inside tetherkitnext::rndis would shadow it,
/// making it impossible to write "may fail but returns nothing" signatures within this namespace.
enum class StatusCode : std::uint32_t {
  // ---- Success / pending ----
  kSuccess = 0x0000'0000U,
  kPending = 0x0000'0103U,

  // ---- Informational ----
  kNotRecognized = 0x0001'0001U,
  kNotCopied = 0x0001'0002U,
  kNotAccepted = 0x0001'0003U,
  kCallActive = 0x0001'0007U,

  // ---- Indications (0x4001xxxx, appear in INDICATE_STATUS_MSG) ----
  kOnline = 0x4001'0003U,
  kResetStart = 0x4001'0004U,
  kResetEnd = 0x4001'0005U,
  kRingStatus = 0x4001'0006U,
  kClosed = 0x4001'0007U,
  kWanLineUp = 0x4001'0008U,
  kWanLineDown = 0x4001'0009U,
  kWanFragment = 0x4001'000AU,
  kMediaConnect = 0x4001'000BU,     ///< Link up -- we use this to set feth UP.
  kMediaDisconnect = 0x4001'000CU,  ///< Link down.
  kHardwareLineUp = 0x4001'000DU,
  kHardwareLineDown = 0x4001'000EU,
  kInterfaceUp = 0x4001'000FU,
  kInterfaceDown = 0x4001'0010U,
  kMediaBusy = 0x4001'0011U,
  kMediaSpecificIndication = 0x4001'0012U,
  kLinkSpeedChange = 0x4001'0013U,
  kNetworkChange = 0x4001'0018U,

  // ---- Warnings (0x8xxxxxxx) ----
  kBufferOverflow = 0x8000'0005U,
  kNotResettable = 0x8001'0001U,
  kSoftErrors = 0x8001'0003U,
  kHardErrors = 0x8001'0004U,

  // ---- Errors (0xCxxxxxxx) ----
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

/// Whether it is a failure status (0xC0000000 and above are errors by NDIS convention).
[[nodiscard]] constexpr bool IsFailure(std::uint32_t status) noexcept {
  return (status & 0xC000'0000U) == 0xC000'0000U;
}

/// Readable name of a status code; an unknown value returns empty.
[[nodiscard]] std::string_view StatusName(std::uint32_t status) noexcept;

// =============================================================================
// OID
// =============================================================================

enum class Oid : std::uint32_t {
  // ---- General (OID_GEN_*) ----
  kGenSupportedList = 0x0001'0101U,      ///< Variable length: N LE32 OIDs. When querying, in_len must be 0.
  kGenHardwareStatus = 0x0001'0102U,     ///< LE32: 0=Ready 1=Initializing 2=Reset 3=Closing 4=NotReady
  kGenMediaSupported = 0x0001'0103U,     ///< LE32 array.
  kGenMediaInUse = 0x0001'0104U,         ///< LE32 array.
  kGenMaximumFrameSize = 0x0001'0106U,   ///< LE32: **excluding** the 14-byte Ethernet header, typically 1500.
  kGenLinkSpeed = 0x0001'0107U,          ///< LE32: unit is **100 bps**; 0 when disconnected.
  kGenTransmitBlockSize = 0x0001'010AU,  ///< LE32
  kGenReceiveBlockSize = 0x0001'010BU,   ///< LE32
  kGenVendorId = 0x0001'010CU,           ///< LE32: the low 24 bits are the IEEE OUI.
  kGenVendorDescription = 0x0001'010DU,  ///< Variable-length ASCII, **NUL termination not guaranteed**.
  kGenCurrentPacketFilter = 0x0001'010EU,  ///< LE32 bit mask, readable and writable.
  kGenMaximumTotalSize = 0x0001'0111U,   ///< LE32: maximum total frame length including headers.
  kGenMacOptions = 0x0001'0113U,         ///< LE32: MacOption bit mask.
  kGenMediaConnectStatus = 0x0001'0114U,  ///< LE32: **0=connected, 1=disconnected** (note that 0 means connected).
  kGenPhysicalMedium = 0x0001'0202U,     ///< LE32: PhysicalMedium; **optional OID**, failure is not fatal.

  // ---- Statistics (OID_GEN_*_OK/ERROR): may return 4 or 8 bytes, both must be accepted ----
  kGenXmitOk = 0x0002'0101U,
  kGenRcvOk = 0x0002'0102U,
  kGenXmitError = 0x0002'0103U,
  kGenRcvError = 0x0002'0104U,
  kGenRcvNoBuffer = 0x0002'0105U,

  // ---- 802.3 specific ----
  kEthernetPermanentAddress = 0x0101'0101U,  ///< 6-byte MAC.
  kEthernetCurrentAddress = 0x0101'0102U,    ///< 6-byte MAC.
  kEthernetMulticastList = 0x0101'0103U,     ///< SET: array of 6N bytes of MACs. **Do not rely on QUERY**.
  kEthernetMaximumListSize = 0x0101'0104U,   ///< LE32: maximum number of entries in the multicast table.
  kEthernetMacOptions = 0x0101'0105U,        ///< LE32: Ethernet802_3MacOption bit mask.
};

/// Readable name of an OID; an unknown value returns empty.
[[nodiscard]] std::string_view OidName(std::uint32_t oid) noexcept;

/// Whether the return value of this OID is variable-length.
///
/// This distinction is crucial: for **fixed-length** OIDs, the QUERY's InformationBufferLength must be set to
/// >= the expected response length, with the same number of zero bytes appended at the end of the message, otherwise Microsoft's ActiveSync implementation replies with
/// RNDIS_STATUS_INVALID_LENGTH; while for **variable-length** OIDs 0 must be passed, otherwise the same error occurs.
/// This undocumented behavior was discovered only by sniffing the Windows driver of ActiveSync 4.1.
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
// Packet filter bit masks (NDIS_PACKET_TYPE_*)
// =============================================================================

enum class PacketFilter : std::uint32_t {
  kDirected = 0x0000'0001U,       ///< Destination MAC equals this machine's address.
  kMulticast = 0x0000'0002U,      ///< Multicast addresses that are in the multicast table.
  kAllMulticast = 0x0000'0004U,   ///< All multicast, ignoring the multicast table.
  kBroadcast = 0x0000'0008U,      ///< ff:ff:ff:ff:ff:ff
  kSourceRouting = 0x0000'0010U,
  kPromiscuous = 0x0000'0020U,    ///< All frames.
  kSmt = 0x0000'0040U,
  kAllLocal = 0x0000'0080U,
  kGroup = 0x0000'1000U,
  kAllFunctional = 0x0000'2000U,
  kFunctional = 0x0000'4000U,
  kMacFrame = 0x0000'8000U,
};

/// The packet filter combination used by this project, consistent with Linux rndis_host (0x0000002D).
///
/// Why enable PROMISCUOUS: we bridge the device as a "network cable" to feth,
/// and the host side may send and receive frames of arbitrary MACs (for example when an upper layer bridges again or VMs are involved), so it cannot only accept
/// directed. The same goes for ALL_MULTICAST -- it avoids maintaining a multicast table while also missing IPv6 neighbor discovery.
inline constexpr std::uint32_t kDefaultPacketFilter =
    ToRaw(PacketFilter::kDirected) | ToRaw(PacketFilter::kBroadcast) |
    ToRaw(PacketFilter::kAllMulticast) | ToRaw(PacketFilter::kPromiscuous);
static_assert(kDefaultPacketFilter == 0x0000'002DU);

// =============================================================================
// Media, physical media, device flags
// =============================================================================

enum class Medium : std::uint32_t {
  kEthernet = 0x0000'0000U,  ///< 802.3; also UNSPECIFIED. This project supports only this one.
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

/// Bit mask of DeviceFlags in INITIALIZE_CMPLT.
enum class DeviceFlags : std::uint32_t {
  kConnectionless = 0x0000'0001U,      ///< Ethernet devices should set this bit.
  kConnectionOriented = 0x0000'0002U,  ///< CONDIS, not supported by this project.
  kRawData = 0x0000'0004U,
};

/// Values of OID_GEN_MEDIA_CONNECT_STATUS. **Note that 0 means connected.**
enum class MediaState : std::uint32_t {
  kConnected = 0,
  kDisconnected = 1,
};

/// Bit mask of OID_GEN_MAC_OPTIONS.
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
// USB layer constants (control channel and descriptor recognition)
// =============================================================================

/// bRequest of SEND_ENCAPSULATED_COMMAND (CDC spec).
inline constexpr std::uint8_t kRequestSendEncapsulatedCommand = 0x00;
/// bRequest of GET_ENCAPSULATED_RESPONSE.
inline constexpr std::uint8_t kRequestGetEncapsulatedResponse = 0x01;

/// bmRequestType: host -> device, class request, interface recipient.
inline constexpr std::uint8_t kControlOutRequestType = 0x21;
/// bmRequestType: device -> host, class request, interface recipient.
inline constexpr std::uint8_t kControlInRequestType = 0xA1;

/// Notification on the interrupt IN endpoint: 8 bytes = two LE32.
///
/// Note this is **not** CDC's usb_cdc_notification structure, but RNDIS's own format:
///   offset 0..3 = Notification (0x00000001 = RESPONSE_AVAILABLE)
///   offset 4..7 = Reserved (0)
inline constexpr std::uint32_t kNotificationBytes = 8;
inline constexpr std::uint32_t kNotificationResponseAvailable = 0x0000'0001U;
inline constexpr std::uint32_t kNotificationValueOffset = 0;
inline constexpr std::uint32_t kNotificationReservedOffset = 4;

/// The USB interface class/subclass/protocol triple, used to recognize RNDIS devices.
struct InterfaceSignature {
  std::uint8_t interface_class;
  std::uint8_t interface_subclass;
  std::uint8_t interface_protocol;

  [[nodiscard]] constexpr bool operator==(const InterfaceSignature&) const = default;
};

/// All known forms of the RNDIS communications-class interface (four in measured use).
///
/// (a) Standard MS RNDIS: CDC Communication / ACM / vendor-specific protocol
/// (b) ActiveSync (Windows Mobile 5 / Windows Phone): USB_CLASS_MISC
/// (c) Wireless RNDIS (phone USB tethering, WWAN modules): Wireless Controller / RF / RNDIS
/// (d) Novatel/Verizon USB730L variant
inline constexpr InterfaceSignature kControlSignatureMicrosoft{0x02, 0x02, 0xFF};
inline constexpr InterfaceSignature kControlSignatureActiveSync{0xEF, 0x01, 0x01};
inline constexpr InterfaceSignature kControlSignatureWireless{0xE0, 0x01, 0x03};
inline constexpr InterfaceSignature kControlSignatureNovatel{0xEF, 0x04, 0x01};

/// RNDIS data-class interface: CDC Data, no subclass, no protocol.
inline constexpr InterfaceSignature kDataSignature{0x0A, 0x00, 0x00};

/// Whether it is a known RNDIS communications-class interface signature.
[[nodiscard]] constexpr bool IsRndisControlSignature(const InterfaceSignature& signature) noexcept {
  return signature == kControlSignatureMicrosoft || signature == kControlSignatureActiveSync ||
         signature == kControlSignatureWireless || signature == kControlSignatureNovatel;
}

// =============================================================================
// Frame size derivation
// =============================================================================

/// Ethernet header length.
inline constexpr std::uint32_t kEthernetHeaderBytes = 14;

/// The RNDIS data channel's "hard header" = Ethernet header + PACKET_MSG header.
///
/// Linux's hard_header_len is this 58, and the derivation of MaxTransferSize is based on it.
inline constexpr std::uint32_t kHardHeaderBytes = kEthernetHeaderBytes + kPacketMsgHeaderBytes;
static_assert(kHardHeaderBytes == 58);

/// The standard Ethernet MTU.
inline constexpr std::uint32_t kDefaultMtu = 1500;

/// Given an MTU, computes the maximum number of bytes one frame occupies on USB (including the RNDIS header and the Ethernet header).
[[nodiscard]] constexpr std::uint32_t HardMtuFor(std::uint32_t mtu) noexcept {
  return mtu + kHardHeaderBytes;
}

/// Computes the MaxTransferSize that should be claimed in INITIALIZE_MSG.
///
/// Follows Linux's algorithm: `(hard_mtu + maxpacket + 1) & ~(maxpacket - 1)`,
/// i.e. round up to an integer multiple of the endpoint max packet size and leave one packet of margin.
/// At high speed (maxpacket=512) an MTU of 1500 yields 2048; at full speed (64) it yields 1600.
[[nodiscard]] constexpr std::uint32_t MaxTransferSizeFor(std::uint32_t mtu,
                                                         std::uint32_t endpoint_max_packet) noexcept {
  const std::uint32_t hard_mtu = HardMtuFor(mtu);
  return (hard_mtu + endpoint_max_packet + 1) & ~(endpoint_max_packet - 1);
}

static_assert(MaxTransferSizeFor(1500, 512) == 2048);
static_assert(MaxTransferSizeFor(1500, 64) == 1600);

/// Hard upper limit of MaxTransferSize for USB 1.1 devices (required by the spec).
inline constexpr std::uint32_t kUsb11MaxTransferSize = 0x4000;

}  // namespace tetherkitnext::rndis
