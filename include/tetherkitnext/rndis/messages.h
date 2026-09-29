// Encoding and decoding of RNDIS control messages.
//
// Design trade-off: **do not map the wire format directly with packed structs**, but use "logical structs + explicit
// offset reads/writes". There are three reasons:
//   1. The wire format is little-endian, and packed structs fail silently on big-endian machines;
//   2. The start offset of an RNDIS message in the USB buffer is only guaranteed to be 4-byte aligned, and member access of a packed struct
//      at some offsets is undefined behavior (-fsanitize=alignment reports it);
//   3. The base point of the offset fields follows the counter-intuitive rule "message start + 8" (see rule 2 in the file header of protocol.h),
//      and explicit Encode/Decode functions concentrate this conversion in one place and add tests,
//      whereas a struct mapping would make every use site remember the +/-8 itself.
//
// Decode functions all perform **complete bounds and self-consistency checks**: control channel data comes from an external device,
// and must be assumed potentially malicious or buggy. A failed check returns Error rather than crashing or reading out of bounds.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/rndis/protocol.h"

namespace tetherkitnext::rndis {

/// 6-byte Ethernet MAC address.
using MacAddress = std::array<std::uint8_t, 6>;

/// Renders a MAC as "aa:bb:cc:dd:ee:ff".
[[nodiscard]] std::array<char, 18> FormatMac(const MacAddress& mac) noexcept;

// =============================================================================
// Common header access
// =============================================================================

/// The common header of any RNDIS message.
struct MessageHeader {
  std::uint32_t message_type = 0;
  std::uint32_t message_length = 0;
};

/// Reads the common header. Returns an error if the buffer is smaller than 8 bytes.
[[nodiscard]] Result<MessageHeader> DecodeMessageHeader(std::span<const std::byte> buffer);

// =============================================================================
// INITIALIZE
// =============================================================================

struct InitializeRequest {
  std::uint32_t request_id = 0;
  std::uint32_t major_version = kMajorVersion;
  std::uint32_t minor_version = kMinorVersion;
  /// The maximum number of bytes the host can receive from bulk IN at one time, computed with MaxTransferSizeFor().
  std::uint32_t max_transfer_size = 0;
};

/// Encodes INITIALIZE_MSG. The buffer must be at least kInitializeMsgBytes bytes; returns the length written.
[[nodiscard]] Result<std::uint32_t> Encode(const InitializeRequest& request,
                                          std::span<std::byte> buffer);

struct InitializeComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
  std::uint32_t major_version = 0;
  std::uint32_t minor_version = 0;
  std::uint32_t device_flags = 0;
  std::uint32_t medium = 0;
  /// Upper limit on the number of PACKET_MSGs the device can carry in one transfer. Most Android gadgets report 1,
  /// while those with the Qualcomm aggregation patch report 3 or more. 0 is treated as 1.
  std::uint32_t max_packets_per_message = 0;
  /// Maximum bytes the device can receive at once -- determines the upper limit of host-side TX aggregation.
  std::uint32_t max_transfer_size = 0;
  /// Host -> device alignment requirement; alignment bytes = 1 << this value, legal upper bound 7.
  std::uint32_t packet_alignment_factor = 0;
  std::uint32_t af_list_offset = 0;
  std::uint32_t af_list_size = 0;
};

[[nodiscard]] Result<InitializeComplete> DecodeInitializeComplete(std::span<const std::byte> buffer);

/// Parameters derived from INITIALIZE_CMPLT that the data path actually uses.
struct NegotiatedParameters {
  std::uint32_t device_max_transfer_size = 0;  ///< Upper limit in bytes for TX aggregation.
  std::uint32_t max_packets_per_message = 1;   ///< Upper limit in packets for TX aggregation.
  std::uint32_t tx_alignment_bytes = 1;        ///< Per-packet alignment for host -> device.
  std::uint32_t mtu = kDefaultMtu;             ///< The MTU finally adopted.
  bool connectionless = true;
};

/// Validates and normalizes INITIALIZE_CMPLT, yielding parameters directly usable by the data path.
///
/// All known device quirks are handled together here:
///   * MaxPacketsPerMessage of 0 -> treated as 1;
///   * PacketAlignmentFactor > 7 -> clamped to 7 (a protocol violation; otherwise absurd padding would be computed);
///   * MaxTransferSize smaller than one full frame (hard_mtu) -> back-derive and lower the MTU;
///     too small (<= 58, cannot fit the headers) -> report an error;
///   * MaxTransferSize reporting 8KB/16KB jumbo frames (habit of WinCE / Windows Mobile) ->
///     do not follow blindly; clamp to our own buffer limit.
[[nodiscard]] Result<NegotiatedParameters> Negotiate(const InitializeComplete& complete,
                                                     std::uint32_t requested_mtu,
                                                     std::uint32_t host_transfer_size_limit);

// =============================================================================
// HALT
// =============================================================================

/// Encodes HALT_MSG. **The device does not reply**; once sent, it can be considered to have entered uninitialized.
[[nodiscard]] Result<std::uint32_t> EncodeHalt(std::uint32_t request_id,
                                               std::span<std::byte> buffer);

// =============================================================================
// QUERY / SET
// =============================================================================

struct QueryRequest {
  std::uint32_t request_id = 0;
  Oid oid = Oid::kGenSupportedList;
  /// Expected response size in bytes.
  ///
  /// For **fixed-length** OIDs it must be >= the expected response length, and the same number of zero bytes must be padded at the end of the message;
  /// for **variable-length** OIDs it must be 0. This undocumented requirement comes from Microsoft's ActiveSync implementation,
  /// and violating it yields RNDIS_STATUS_INVALID_LENGTH. Encode automatically corrects by IsVariableLengthOid(),
  /// so the caller can fill 0 and let it decide on its own.
  std::uint32_t expected_response_bytes = 0;
};

/// Encodes QUERY_MSG (including trailing padding decided automatically by OID type).
[[nodiscard]] Result<std::uint32_t> Encode(const QueryRequest& request,
                                          std::span<std::byte> buffer);

struct QueryComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
  /// A view of the information buffer inside `buffer`, with the same lifetime as `buffer`.
  std::span<const std::byte> information;
};

[[nodiscard]] Result<QueryComplete> DecodeQueryComplete(std::span<const std::byte> buffer);

struct SetRequest {
  std::uint32_t request_id = 0;
  Oid oid = Oid::kGenCurrentPacketFilter;
  std::span<const std::byte> information;
};

/// Encodes SET_MSG.
[[nodiscard]] Result<std::uint32_t> Encode(const SetRequest& request, std::span<std::byte> buffer);

/// Convenience entry point: SET one LE32 value (the most common, e.g. setting the packet filter).
[[nodiscard]] Result<std::uint32_t> EncodeSetUint32(std::uint32_t request_id, Oid oid,
                                                    std::uint32_t value,
                                                    std::span<std::byte> buffer);

struct SetComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
};

[[nodiscard]] Result<SetComplete> DecodeSetComplete(std::span<const std::byte> buffer);

// =============================================================================
// RESET
// =============================================================================

/// Encodes RESET_MSG.
///
/// **Note: RESET_MSG has no RequestId field** (offset 8 is Reserved), so responses cannot be paired by
/// ID, and only one RESET can be in flight at a time.
[[nodiscard]] Result<std::uint32_t> EncodeReset(std::span<std::byte> buffer);

struct ResetComplete {
  std::uint32_t status = 0;
  /// Non-zero means addressing information (packet filter, multicast table) was lost in the reset, and the host must resend the corresponding SETs.
  bool addressing_reset = false;
};

[[nodiscard]] Result<ResetComplete> DecodeResetComplete(std::span<const std::byte> buffer);

// =============================================================================
// KEEPALIVE
// =============================================================================

[[nodiscard]] Result<std::uint32_t> EncodeKeepAlive(std::uint32_t request_id,
                                                    std::span<std::byte> buffer);

struct KeepAliveComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
};

[[nodiscard]] Result<KeepAliveComplete> DecodeKeepAliveComplete(std::span<const std::byte> buffer);

/// Encodes KEEPALIVE_CMPLT.
///
/// It is needed because the **device can also proactively send KEEPALIVE_MSG**, in which case the host must reply with
/// KEEPALIVE_CMPLT, otherwise the device may consider the host dead and disconnect.
[[nodiscard]] Result<std::uint32_t> EncodeKeepAliveComplete(std::uint32_t request_id,
                                                            std::uint32_t status,
                                                            std::span<std::byte> buffer);

// =============================================================================
// INDICATE_STATUS
// =============================================================================

struct IndicateStatus {
  std::uint32_t status = 0;
  /// View of the status buffer; empty in most cases (MEDIA_CONNECT/DISCONNECT carry no payload).
  std::span<const std::byte> status_buffer;
  /// If the status buffer happens to be an 8-byte RNDIS_Diagnostic_Info, these two fields are filled.
  bool has_diagnostic_info = false;
  std::uint32_t diagnostic_status = 0;
  std::uint32_t diagnostic_error_offset = 0;
};

/// Decodes INDICATE_STATUS_MSG.
///
/// **The base point of StatusBufferOffset is contradictory in the spec** (MS documentation says message start, while the QUERY/SET of the same
/// family are all message start + 8). This implementation tolerates both in the following order:
///   1. If the length is 0 -- no payload, succeed directly (the vast majority of cases);
///   2. First interpret with "base = message start + 8"; adopt it if it falls within the message range;
///   3. Otherwise interpret with "base = message start";
///   4. If both are out of bounds -- discard the status buffer but **still return success**, because the status itself is useful,
///      and the link must not be dropped just because an optional payload cannot be parsed.
[[nodiscard]] Result<IndicateStatus> DecodeIndicateStatus(std::span<const std::byte> buffer);

// =============================================================================
// OID payload parsing
// =============================================================================

/// Takes an LE32 from the information buffer of QUERY_CMPLT.
[[nodiscard]] Result<std::uint32_t> ParseUint32(std::span<const std::byte> information);

/// Takes a counter value from the information buffer.
///
/// Statistics OIDs (OID_GEN_XMIT_OK etc.) may return 4 or 8 bytes; both must be accepted.
[[nodiscard]] Result<std::uint64_t> ParseCounter(std::span<const std::byte> information);

/// Takes a 6-byte MAC from the information buffer.
[[nodiscard]] Result<MacAddress> ParseMac(std::span<const std::byte> information);

}  // namespace tetherkitnext::rndis
