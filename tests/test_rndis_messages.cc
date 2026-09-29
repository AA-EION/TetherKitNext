// Unit tests of RNDIS control message encoding/decoding.
//
// Key coverage:
//   1. The base point of offset fields is "message start + 8" -- this is RNDIS's number one trap;
//   2. RESET_MSG / RESET_CMPLT **have no** RequestId, and their field offsets differ from other messages;
//   3. The InformationBufferLength rules for variable-length / fixed-length OIDs are opposite;
//   4. The base point of INDICATE_STATUS's StatusBufferOffset is contradictory in the spec;
//   5. Normalization of device quirks (MaxPacketsPerMessage=0, alignment factor out of range,
//      MaxTransferSize too large or too small);
//   6. All malformed inputs must return an error rather than crash or read out of bounds.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <doctest.h>

#include "tetherkitnext/common/byte_order.h"
#include "tetherkitnext/rndis/messages.h"
#include "tetherkitnext/rndis/protocol.h"

using namespace tetherkitnext;        // NOLINT(google-build-using-namespace)
using namespace tetherkitnext::rndis;  // NOLINT(google-build-using-namespace)

namespace {

/// Builds an RNDIS message: write Type/Length first, then the caller fills in the fields.
std::vector<std::byte> MakeMessage(MessageType type, std::uint32_t message_length,
                                   std::uint32_t buffer_bytes = 0) {
  std::vector<std::byte> buffer(buffer_bytes == 0 ? message_length : buffer_bytes);
  StoreLe32(buffer.data() + kMessageTypeOffset, ToRaw(type));
  StoreLe32(buffer.data() + kMessageLengthOffset, message_length);
  return buffer;
}

/// Builds an INITIALIZE_CMPLT of the kind a mainline Linux gadget (Android) would send.
std::vector<std::byte> MakeInitializeComplete(std::uint32_t max_transfer_size = 2048,
                                              std::uint32_t max_packets = 1,
                                              std::uint32_t alignment_factor = 0,
                                              std::uint32_t status = 0,
                                              std::uint32_t medium = 0,
                                              std::uint32_t device_flags = 1) {
  auto buffer = MakeMessage(MessageType::kInitializeComplete, kInitializeCmpltBytes);
  std::byte* base = buffer.data();
  StoreLe32(base + kInitializeCmpltRequestIdOffset, 0x1234'5678U);
  StoreLe32(base + kInitializeCmpltStatusOffset, status);
  StoreLe32(base + kInitializeCmpltMajorVersionOffset, 1);
  StoreLe32(base + kInitializeCmpltMinorVersionOffset, 0);
  StoreLe32(base + kInitializeCmpltDeviceFlagsOffset, device_flags);
  StoreLe32(base + kInitializeCmpltMediumOffset, medium);
  StoreLe32(base + kInitializeCmpltMaxPacketsPerMessageOffset, max_packets);
  StoreLe32(base + kInitializeCmpltMaxTransferSizeOffset, max_transfer_size);
  StoreLe32(base + kInitializeCmpltPacketAlignmentFactorOffset, alignment_factor);
  return buffer;
}

}  // namespace

TEST_SUITE("rndis.messages") {

// ---------------------------------------------------------------------------
// Constant self-checks
// ---------------------------------------------------------------------------

TEST_CASE("协议常量与规范一致") {
  CHECK(ToRaw(MessageType::kPacket) == 0x0000'0001U);
  CHECK(ToRaw(MessageType::kInitialize) == 0x0000'0002U);
  CHECK(ToRaw(MessageType::kInitializeComplete) == 0x8000'0002U);
  CHECK(ToRaw(MessageType::kHalt) == 0x0000'0003U);
  CHECK(ToRaw(MessageType::kQueryComplete) == 0x8000'0004U);
  CHECK(ToRaw(MessageType::kResetComplete) == 0x8000'0006U);
  CHECK(ToRaw(MessageType::kIndicateStatus) == 0x0000'0007U);
  CHECK(ToRaw(MessageType::kKeepAliveComplete) == 0x8000'0008U);

  // A few status codes that are the easiest to misremember.
  CHECK(ToRaw(StatusCode::kNotSupported) == 0xC000'00BBU);
  CHECK(ToRaw(StatusCode::kInvalidLength) == 0xC001'0014U);
  CHECK(ToRaw(StatusCode::kInvalidData) == 0xC001'0015U);
  CHECK(ToRaw(StatusCode::kInvalidOid) == 0xC001'0017U);
  CHECK(ToRaw(StatusCode::kMediaConnect) == 0x4001'000BU);
  CHECK(ToRaw(StatusCode::kMediaDisconnect) == 0x4001'000CU);

  CHECK(ToRaw(Oid::kGenCurrentPacketFilter) == 0x0001'010EU);
  CHECK(ToRaw(Oid::kGenMediaConnectStatus) == 0x0001'0114U);
  CHECK(ToRaw(Oid::kEthernetPermanentAddress) == 0x0101'0101U);
  CHECK(ToRaw(Oid::kGenPhysicalMedium) == 0x0001'0202U);

  CHECK(kDefaultPacketFilter == 0x0000'002DU);
  CHECK(kHardHeaderBytes == 58);
}

TEST_CASE("偏移基准点常量：DataOffset 内联值必须是 36 而不是 44") {
  // This is RNDIS's number one trap: the base point of offset fields is message start +8.
  CHECK(kPacketInlineDataOffset == 36);
  CHECK(kPacketMsgHeaderBytes == 44);
  CHECK(kOffsetFieldBase == 8);
  CHECK(kQuerySetInlineInfoOffset == 20);
  CHECK(kQueryCmpltInlineInfoOffset == 16);
}

TEST_CASE("MaxTransferSize 推导与 Linux 一致") {
  CHECK(MaxTransferSizeFor(1500, 512) == 2048);  // high speed
  CHECK(MaxTransferSizeFor(1500, 64) == 1600);   // full speed
  CHECK(HardMtuFor(1500) == 1558);
}

TEST_CASE("状态码与 OID 的名字可查") {
  CHECK(StatusName(0xC000'00BBU) == "RNDIS_STATUS_NOT_SUPPORTED");
  CHECK(StatusName(0x4001'000BU) == "RNDIS_STATUS_MEDIA_CONNECT");
  CHECK(StatusName(0xDEAD'BEEFU).empty());
  CHECK(OidName(0x0001'010EU) == "OID_GEN_CURRENT_PACKET_FILTER");
  CHECK(OidName(0xDEAD'BEEFU).empty());
  CHECK(MessageTypeName(0x8000'0002U) == "REMOTE_NDIS_INITIALIZE_CMPLT");
  // The CONDIS message family must be recognizable (even though unsupported), rather than reported as "unknown".
  CHECK(MessageTypeName(0x0000'8001U) == "RNDIS_MSG_MP_CREATE_VC");
  CHECK(IsCondis(0x0000'8001U));
  CHECK_FALSE(IsCondis(ToRaw(MessageType::kPacket)));
}

TEST_CASE("变长与定长 OID 的区分") {
  CHECK(IsVariableLengthOid(Oid::kGenSupportedList));
  CHECK(IsVariableLengthOid(Oid::kGenVendorDescription));
  CHECK_FALSE(IsVariableLengthOid(Oid::kEthernetPermanentAddress));
  CHECK_FALSE(IsVariableLengthOid(Oid::kGenCurrentPacketFilter));
}

// ---------------------------------------------------------------------------
// INITIALIZE
// ---------------------------------------------------------------------------

TEST_CASE("编码 INITIALIZE_MSG") {
  std::array<std::byte, 64> buffer{};
  const auto written = Encode(
      InitializeRequest{.request_id = 7, .max_transfer_size = 2048}, buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kInitializeMsgBytes);

  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kInitialize));
  CHECK(LoadLe32(buffer.data() + kMessageLengthOffset) == 24);
  CHECK(LoadLe32(buffer.data() + kInitializeRequestIdOffset) == 7);
  CHECK(LoadLe32(buffer.data() + kInitializeMajorVersionOffset) == 1);
  CHECK(LoadLe32(buffer.data() + kInitializeMinorVersionOffset) == 0);
  CHECK(LoadLe32(buffer.data() + kInitializeMaxTransferSizeOffset) == 2048);
}

TEST_CASE("编码 INITIALIZE_MSG 缓冲不足时报错") {
  std::array<std::byte, 16> too_small{};
  CHECK_FALSE(Encode(InitializeRequest{}, too_small).has_value());
}

TEST_CASE("解码 INITIALIZE_CMPLT") {
  const auto buffer = MakeInitializeComplete();
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 0x1234'5678U);
  CHECK(complete->status == 0);
  CHECK(complete->major_version == 1);
  CHECK(complete->max_transfer_size == 2048);
  CHECK(complete->max_packets_per_message == 1);
}

TEST_CASE("解码 INITIALIZE_CMPLT：设备返回失败状态") {
  const auto buffer = MakeInitializeComplete(2048, 1, 0, ToRaw(StatusCode::kFailure));
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE_FALSE(complete.has_value());
  CHECK(complete.error().Code() == static_cast<std::int64_t>(ToRaw(StatusCode::kFailure)));
  // The error string should carry the symbolic name of the status code, to ease troubleshooting.
  CHECK(complete.error().ToString().find("RNDIS_STATUS_FAILURE") != std::string::npos);
}

TEST_CASE("解码 INITIALIZE_CMPLT：畸形输入一律报错不崩溃") {
  SUBCASE("缓冲区太短") {
    std::array<std::byte, 8> tiny{};
    StoreLe32(tiny.data(), ToRaw(MessageType::kInitializeComplete));
    StoreLe32(tiny.data() + 4, kInitializeCmpltBytes);
    CHECK_FALSE(DecodeInitializeComplete(tiny).has_value());
  }
  SUBCASE("消息类型不对") {
    auto buffer = MakeInitializeComplete();
    StoreLe32(buffer.data() + kMessageTypeOffset, ToRaw(MessageType::kQueryComplete));
    CHECK_FALSE(DecodeInitializeComplete(buffer).has_value());
  }
  SUBCASE("MessageLength 超出缓冲") {
    auto buffer = MakeInitializeComplete();
    StoreLe32(buffer.data() + kMessageLengthOffset, 0xFFFF'FFFFU);
    CHECK_FALSE(DecodeInitializeComplete(buffer).has_value());
  }
  SUBCASE("MessageLength 小于最小长度") {
    auto buffer = MakeInitializeComplete();
    StoreLe32(buffer.data() + kMessageLengthOffset, 12);
    CHECK_FALSE(DecodeInitializeComplete(buffer).has_value());
  }
  SUBCASE("空缓冲") {
    CHECK_FALSE(DecodeInitializeComplete({}).has_value());
  }
}

// ---------------------------------------------------------------------------
// Negotiation and normalization of device quirks
// ---------------------------------------------------------------------------

TEST_CASE("协商：Android 主线 gadget 的典型参数") {
  const auto buffer = MakeInitializeComplete(2048, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());

  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->mtu == 1500);
  CHECK(params->device_max_transfer_size == 2048);
  CHECK(params->max_packets_per_message == 1);
  CHECK(params->tx_alignment_bytes == 1);  // factor 0 -> alignment of 1 byte
}

TEST_CASE("协商 quirk：MaxPacketsPerMessage 为 0 时按 1 处理") {
  const auto buffer = MakeInitializeComplete(2048, 0, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->max_packets_per_message == 1);
}

TEST_CASE("协商 quirk：PacketAlignmentFactor 越界时钳到 7") {
  const auto buffer = MakeInitializeComplete(2048, 1, 31);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  // Without clamping, 1u << 31 would give an absurd alignment value.
  CHECK(params->tx_alignment_bytes == 128);
}

TEST_CASE("协商 quirk：高通聚合补丁报 MaxPacketsPerMessage=3") {
  const auto buffer = MakeInitializeComplete(3 * 1580, 3, 2);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->max_packets_per_message == 3);
  CHECK(params->tx_alignment_bytes == 4);  // 1 << 2
}

TEST_CASE("协商 quirk：设备 MaxTransferSize 小于满帧时下调 MTU") {
  // Measured case: the HTC Diamond reports 1536, less than hard_mtu(1558).
  const auto buffer = MakeInitializeComplete(1536, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 65536);
  REQUIRE(params.has_value());
  CHECK(params->mtu == 1536 - kHardHeaderBytes);  // 1478
}

TEST_CASE("协商 quirk：WinCE 报 16KB 巨帧时钳到 host 上限") {
  const auto buffer = MakeInitializeComplete(16384, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  const auto params = Negotiate(*complete, 1500, 4096);
  REQUIRE(params.has_value());
  CHECK(params->device_max_transfer_size == 4096);
  CHECK(params->mtu == 1500);  // still fits a full frame, MTU unchanged
}

TEST_CASE("协商失败：MaxTransferSize 装不下头部") {
  const auto buffer = MakeInitializeComplete(40, 1, 0);
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(Negotiate(*complete, 1500, 65536).has_value());
}

TEST_CASE("协商失败：非以太网介质") {
  const auto buffer =
      MakeInitializeComplete(2048, 1, 0, 0, static_cast<std::uint32_t>(Medium::kWirelessLan));
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(Negotiate(*complete, 1500, 65536).has_value());
}

TEST_CASE("协商失败：面向连接设备") {
  const auto buffer = MakeInitializeComplete(
      2048, 1, 0, 0, 0, static_cast<std::uint32_t>(DeviceFlags::kConnectionOriented));
  const auto complete = DecodeInitializeComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(Negotiate(*complete, 1500, 65536).has_value());
}

// ---------------------------------------------------------------------------
// QUERY / SET
// ---------------------------------------------------------------------------

TEST_CASE("编码 QUERY_MSG：定长 OID 必须带占位长度与尾部零字节") {
  std::array<std::byte, 128> buffer{};
  const auto written = Encode(QueryRequest{.request_id = 3,
                                           .oid = Oid::kEthernetPermanentAddress,
                                           .expected_response_bytes = 48},
                              buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kQuerySetHeaderBytes + 48);

  CHECK(LoadLe32(buffer.data() + kQuerySetOidOffset) == ToRaw(Oid::kEthernetPermanentAddress));
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferLengthOffset) == 48);
  // The base point is message start +8, so the offset value for inline data is 20 rather than 28.
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset) == 20);
  CHECK(LoadLe32(buffer.data() + kQuerySetDeviceVcHandleOffset) == 0);
}

TEST_CASE("编码 QUERY_MSG：变长 OID 的长度必须为 0") {
  // Undocumented behavior of ActiveSync: passing a non-zero length for a variable-length OID gets rejected.
  std::array<std::byte, 128> buffer{};
  const auto written = Encode(QueryRequest{.request_id = 4,
                                           .oid = Oid::kGenSupportedList,
                                           .expected_response_bytes = 256},
                              buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kQuerySetHeaderBytes);
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferLengthOffset) == 0);
  // When the length is 0, the offset must also be written as 0.
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset) == 0);
}

TEST_CASE("解码 QUERY_CMPLT：按 +8 基准点定位信息缓冲区") {
  auto buffer = MakeMessage(MessageType::kQueryComplete, kQueryCmpltHeaderBytes + 6);
  StoreLe32(buffer.data() + kQueryCmpltRequestIdOffset, 9);
  StoreLe32(buffer.data() + kQueryCmpltStatusOffset, 0);
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferLengthOffset, 6);
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferOffsetOffset, kQueryCmpltInlineInfoOffset);
  const std::array<std::uint8_t, 6> mac{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  for (std::size_t i = 0; i < mac.size(); ++i) {
    buffer[kQueryCmpltHeaderBytes + i] = std::byte{mac[i]};
  }

  const auto complete = DecodeQueryComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 9);
  REQUIRE(complete->information.size() == 6);

  const auto parsed = ParseMac(complete->information);
  REQUIRE(parsed.has_value());
  CHECK(*parsed == MacAddress{0x02, 0x11, 0x22, 0x33, 0x44, 0x55});
}

TEST_CASE("解码 QUERY_CMPLT：设备返回不支持时不算致命错误") {
  // An optional OID (such as OID_GEN_PHYSICAL_MEDIUM) returning NOT_SUPPORTED is a normal situation,
  // and decoding must return successfully and hand the status to the caller to judge.
  auto buffer = MakeMessage(MessageType::kQueryComplete, kQueryCmpltHeaderBytes);
  StoreLe32(buffer.data() + kQueryCmpltStatusOffset, ToRaw(StatusCode::kNotSupported));
  const auto complete = DecodeQueryComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->status == ToRaw(StatusCode::kNotSupported));
  CHECK(complete->information.empty());
}

TEST_CASE("解码 QUERY_CMPLT：信息缓冲区越界必须报错") {
  auto buffer = MakeMessage(MessageType::kQueryComplete, kQueryCmpltHeaderBytes + 4);
  StoreLe32(buffer.data() + kQueryCmpltStatusOffset, 0);
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferLengthOffset, 1024);  // lie
  StoreLe32(buffer.data() + kQueryCmpltInfoBufferOffsetOffset, kQueryCmpltInlineInfoOffset);
  CHECK_FALSE(DecodeQueryComplete(buffer).has_value());
}

TEST_CASE("编码 SET_MSG 与 SET 一个 LE32") {
  std::array<std::byte, 64> buffer{};
  const auto written = EncodeSetUint32(11, Oid::kGenCurrentPacketFilter, kDefaultPacketFilter,
                                       buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kQuerySetHeaderBytes + 4);
  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kSet));
  CHECK(LoadLe32(buffer.data() + kQuerySetOidOffset) == ToRaw(Oid::kGenCurrentPacketFilter));
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferLengthOffset) == 4);
  CHECK(LoadLe32(buffer.data() + kQuerySetInfoBufferOffsetOffset) == 20);
  CHECK(LoadLe32(buffer.data() + kQuerySetHeaderBytes) == kDefaultPacketFilter);
}

TEST_CASE("解码 SET_CMPLT") {
  auto buffer = MakeMessage(MessageType::kSetComplete, kSetCmpltBytes);
  StoreLe32(buffer.data() + kSetCmpltRequestIdOffset, 11);
  StoreLe32(buffer.data() + kSetCmpltStatusOffset, 0);
  const auto complete = DecodeSetComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 11);
  CHECK(complete->status == 0);
}

// ---------------------------------------------------------------------------
// RESET -- field offsets differ from other messages
// ---------------------------------------------------------------------------

TEST_CASE("编码 RESET_MSG：offset 8 是 Reserved 而非 RequestId") {
  std::array<std::byte, 32> buffer{};
  buffer[kResetReservedOffset] = std::byte{0xFF};  // pollute first, to verify it gets zeroed
  const auto written = EncodeReset(buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kResetMsgBytes);
  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kReset));
  CHECK(LoadLe32(buffer.data() + kMessageLengthOffset) == 12);
  CHECK(LoadLe32(buffer.data() + kResetReservedOffset) == 0);
}

TEST_CASE("解码 RESET_CMPLT：Status 在 offset 8 而不是 12") {
  auto buffer = MakeMessage(MessageType::kResetComplete, kResetCmpltBytes);
  StoreLe32(buffer.data() + kResetCmpltStatusOffset, 0);
  StoreLe32(buffer.data() + kResetCmpltAddressingResetOffset, 1);

  const auto complete = DecodeResetComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK(complete->status == 0);
  // AddressingReset non-zero -> the host must resend SET OID_GEN_CURRENT_PACKET_FILTER.
  CHECK(complete->addressing_reset);
}

TEST_CASE("解码 RESET_CMPLT：AddressingReset 为 0 时无需重放") {
  auto buffer = MakeMessage(MessageType::kResetComplete, kResetCmpltBytes);
  StoreLe32(buffer.data() + kResetCmpltAddressingResetOffset, 0);
  const auto complete = DecodeResetComplete(buffer);
  REQUIRE(complete.has_value());
  CHECK_FALSE(complete->addressing_reset);
}

// ---------------------------------------------------------------------------
// KEEPALIVE
// ---------------------------------------------------------------------------

TEST_CASE("KEEPALIVE 往返") {
  std::array<std::byte, 32> request{};
  const auto written = EncodeKeepAlive(42, request);
  REQUIRE(written.has_value());
  CHECK(written.value() == kKeepAliveMsgBytes);
  CHECK(LoadLe32(request.data() + kKeepAliveRequestIdOffset) == 42);

  auto response = MakeMessage(MessageType::kKeepAliveComplete, kKeepAliveCmpltBytes);
  StoreLe32(response.data() + kKeepAliveCmpltRequestIdOffset, 42);
  const auto complete = DecodeKeepAliveComplete(response);
  REQUIRE(complete.has_value());
  CHECK(complete->request_id == 42);
}

TEST_CASE("host 也要能编码 KEEPALIVE_CMPLT（设备可主动发起保活）") {
  std::array<std::byte, 32> buffer{};
  const auto written = EncodeKeepAliveComplete(77, ToRaw(StatusCode::kSuccess), buffer);
  REQUIRE(written.has_value());
  CHECK(written.value() == kKeepAliveCmpltBytes);
  CHECK(LoadLe32(buffer.data() + kMessageTypeOffset) == ToRaw(MessageType::kKeepAliveComplete));
  CHECK(LoadLe32(buffer.data() + kKeepAliveCmpltRequestIdOffset) == 77);
  CHECK(LoadLe32(buffer.data() + kKeepAliveCmpltStatusOffset) == 0);
}

// ---------------------------------------------------------------------------
// INDICATE_STATUS -- tolerant handling of the contradictory base point
// ---------------------------------------------------------------------------

TEST_CASE("解码 INDICATE_STATUS：MEDIA_CONNECT 无负载") {
  auto buffer = MakeMessage(MessageType::kIndicateStatus, kIndicateStatusHeaderBytes);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kMediaConnect));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, 0);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  CHECK(indication->status == ToRaw(StatusCode::kMediaConnect));
  CHECK(indication->status_buffer.empty());
  CHECK_FALSE(indication->has_diagnostic_info);
}

TEST_CASE("解码 INDICATE_STATUS：按 +8 基准点解释状态缓冲区") {
  const std::uint32_t total = kIndicateStatusHeaderBytes + kDiagnosticInfoBytes;
  auto buffer = MakeMessage(MessageType::kIndicateStatus, total);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kInvalidData));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, kDiagnosticInfoBytes);
  // +8 base point: 20 - 8 = 12
  StoreLe32(buffer.data() + kIndicateStatusBufferOffsetOffset,
            kIndicateStatusHeaderBytes - kOffsetFieldBase);
  StoreLe32(buffer.data() + kIndicateStatusHeaderBytes + kDiagnosticInfoDiagStatusOffset,
            ToRaw(StatusCode::kInvalidData));
  StoreLe32(buffer.data() + kIndicateStatusHeaderBytes + kDiagnosticInfoErrorOffsetOffset, 36);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  CHECK(indication->status_buffer.size() == kDiagnosticInfoBytes);
  REQUIRE(indication->has_diagnostic_info);
  CHECK(indication->diagnostic_status == ToRaw(StatusCode::kInvalidData));
  CHECK(indication->diagnostic_error_offset == 36);
}

TEST_CASE("解码 INDICATE_STATUS：按消息起始基准点解释也能工作") {
  // Some devices implement the MS documentation literally (base point = message start).
  const std::uint32_t total = kIndicateStatusHeaderBytes + kDiagnosticInfoBytes;
  auto buffer = MakeMessage(MessageType::kIndicateStatus, total);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kInvalidData));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, kDiagnosticInfoBytes);
  // Base point = message start: fill in 20 directly
  StoreLe32(buffer.data() + kIndicateStatusBufferOffsetOffset, kIndicateStatusHeaderBytes);
  StoreLe32(buffer.data() + kIndicateStatusHeaderBytes + kDiagnosticInfoDiagStatusOffset, 0xAAU);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  // The +8 interpretation would go out of bounds (8+20+8=36 > 28), so fall back to the literal interpretation.
  REQUIRE(indication->status_buffer.size() == kDiagnosticInfoBytes);
  CHECK(indication->diagnostic_status == 0xAAU);
}

TEST_CASE("解码 INDICATE_STATUS：两种基准点都越界时仍返回成功") {
  // Key behavior: the status itself is useful (it may be MEDIA_DISCONNECT),
  // and the link must not be judged dead just because an optional payload cannot be parsed.
  auto buffer = MakeMessage(MessageType::kIndicateStatus, kIndicateStatusHeaderBytes);
  StoreLe32(buffer.data() + kIndicateStatusStatusOffset, ToRaw(StatusCode::kMediaDisconnect));
  StoreLe32(buffer.data() + kIndicateStatusBufferLengthOffset, 4096);  // lie
  StoreLe32(buffer.data() + kIndicateStatusBufferOffsetOffset, 12);

  const auto indication = DecodeIndicateStatus(buffer);
  REQUIRE(indication.has_value());
  CHECK(indication->status == ToRaw(StatusCode::kMediaDisconnect));
  CHECK(indication->status_buffer.empty());
}

TEST_CASE("解码 INDICATE_STATUS：类型不对时报错") {
  auto buffer = MakeMessage(MessageType::kQueryComplete, kIndicateStatusHeaderBytes);
  CHECK_FALSE(DecodeIndicateStatus(buffer).has_value());
}

// ---------------------------------------------------------------------------
// OID payload parsing
// ---------------------------------------------------------------------------

TEST_CASE("ParseUint32 / ParseCounter / ParseMac") {
  std::array<std::byte, 8> payload{};
  StoreLe32(payload.data(), 0x0000'002DU);

  const auto value = ParseUint32(std::span<const std::byte>{payload}.first(4));
  REQUIRE(value.has_value());
  CHECK(*value == 0x0000'002DU);

  SUBCASE("计数器 OID 返回 4 字节") {
    const auto counter = ParseCounter(std::span<const std::byte>{payload}.first(4));
    REQUIRE(counter.has_value());
    CHECK(*counter == 0x0000'002DU);
  }
  SUBCASE("计数器 OID 返回 8 字节") {
    StoreLe64(payload.data(), 0x0000'0001'0000'002DULL);
    const auto counter = ParseCounter(payload);
    REQUIRE(counter.has_value());
    CHECK(*counter == 0x0000'0001'0000'002DULL);
  }
  SUBCASE("计数器 OID 返回 2 字节应报错") {
    CHECK_FALSE(ParseCounter(std::span<const std::byte>{payload}.first(2)).has_value());
  }
  SUBCASE("MAC 长度不足应报错") {
    CHECK_FALSE(ParseMac(std::span<const std::byte>{payload}.first(4)).has_value());
  }
}

TEST_CASE("FormatMac 输出规范格式") {
  const MacAddress mac{0x02, 0x00, 0xAB, 0xCD, 0xEF, 0x01};
  const auto text = FormatMac(mac);
  CHECK(std::string_view{text.data()} == "02:00:ab:cd:ef:01");
}

// ---------------------------------------------------------------------------
// USB descriptor signature recognition
// ---------------------------------------------------------------------------

TEST_CASE("识别四种已知的 RNDIS 通信类接口签名") {
  CHECK(IsRndisControlSignature({0x02, 0x02, 0xFF}));  // standard MS RNDIS
  CHECK(IsRndisControlSignature({0xEF, 0x01, 0x01}));  // ActiveSync
  CHECK(IsRndisControlSignature({0xE0, 0x01, 0x03}));  // wireless RNDIS (phone tethering)
  CHECK(IsRndisControlSignature({0xEF, 0x04, 0x01}));  // Novatel variant

  // A real CDC ACM modem (protocol=0x01) must not be misjudged as RNDIS.
  CHECK_FALSE(IsRndisControlSignature({0x02, 0x02, 0x01}));
  // CDC ECM is not RNDIS either.
  CHECK_FALSE(IsRndisControlSignature({0x02, 0x06, 0x00}));
  CHECK(kDataSignature == InterfaceSignature{0x0A, 0x00, 0x00});
}

}  // TEST_SUITE("rndis.messages")
