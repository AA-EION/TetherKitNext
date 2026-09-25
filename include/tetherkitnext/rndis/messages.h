// RNDIS 控制消息的编解码。
//
// 设计取舍：**不用 packed struct 直接映射线格式**，而是「逻辑结构体 + 显式
// 偏移读写」。理由有三：
//   1. 线格式是小端，packed struct 在大端机上静默出错；
//   2. RNDIS 消息在 USB 缓冲里的起始偏移只保证 4 字节对齐，packed struct 的
//      成员访问在某些偏移上是未定义行为（-fsanitize=alignment 会报）；
//   3. 偏移字段的基准点是「消息起始 + 8」这种反直觉规则（见 protocol.h 文件头
//      规则 2），用显式的 Encode/Decode 函数能把这个换算集中在一处并加测试，
//      而 struct 映射会让每个使用点都得自己记住 ±8。
//
// 解码函数一律做**完整的边界与自洽性校验**：控制通道的数据来自外部设备，
// 必须假定它可能是恶意或有 bug 的。校验失败返回 Error 而非崩溃或读越界。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/rndis/protocol.h"

namespace tetherkitnext::rndis {

/// 6 字节以太网 MAC 地址。
using MacAddress = std::array<std::uint8_t, 6>;

/// 把 MAC 渲染成 "aa:bb:cc:dd:ee:ff"。
[[nodiscard]] std::array<char, 18> FormatMac(const MacAddress& mac) noexcept;

// =============================================================================
// 通用头部访问
// =============================================================================

/// 任意 RNDIS 消息的公共头部。
struct MessageHeader {
  std::uint32_t message_type = 0;
  std::uint32_t message_length = 0;
};

/// 读取公共头部。缓冲区不足 8 字节则返回错误。
[[nodiscard]] Result<MessageHeader> DecodeMessageHeader(std::span<const std::byte> buffer);

// =============================================================================
// INITIALIZE
// =============================================================================

struct InitializeRequest {
  std::uint32_t request_id = 0;
  std::uint32_t major_version = kMajorVersion;
  std::uint32_t minor_version = kMinorVersion;
  /// host 一次能从 bulk IN 接收的最大字节数，用 MaxTransferSizeFor() 算。
  std::uint32_t max_transfer_size = 0;
};

/// 编码 INITIALIZE_MSG。缓冲区至少 kInitializeMsgBytes 字节，返回写入长度。
[[nodiscard]] Result<std::uint32_t> Encode(const InitializeRequest& request,
                                          std::span<std::byte> buffer);

struct InitializeComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
  std::uint32_t major_version = 0;
  std::uint32_t minor_version = 0;
  std::uint32_t device_flags = 0;
  std::uint32_t medium = 0;
  /// 设备一次传输能承载的 PACKET_MSG 个数上限。多数 Android gadget 报 1，
  /// 打了高通聚合补丁的报 3 或更多。为 0 时按 1 处理。
  std::uint32_t max_packets_per_message = 0;
  /// 设备一次能接收的最大字节数 —— 决定 host 侧 TX 聚合的上限。
  std::uint32_t max_transfer_size = 0;
  /// host → device 的对齐要求，对齐字节数 = 1 << 此值，合法上界 7。
  std::uint32_t packet_alignment_factor = 0;
  std::uint32_t af_list_offset = 0;
  std::uint32_t af_list_size = 0;
};

[[nodiscard]] Result<InitializeComplete> DecodeInitializeComplete(std::span<const std::byte> buffer);

/// 由 INITIALIZE_CMPLT 推导出的、数据路径真正要用的参数。
struct NegotiatedParameters {
  std::uint32_t device_max_transfer_size = 0;  ///< TX 聚合的字节上限。
  std::uint32_t max_packets_per_message = 1;   ///< TX 聚合的包数上限。
  std::uint32_t tx_alignment_bytes = 1;        ///< host → device 的每包对齐。
  std::uint32_t mtu = kDefaultMtu;             ///< 最终采用的 MTU。
  bool connectionless = true;
};

/// 校验并归一化 INITIALIZE_CMPLT，得到可直接用于数据路径的参数。
///
/// 这里集中处理全部已知的设备 quirk：
///   * MaxPacketsPerMessage 为 0 → 当 1；
///   * PacketAlignmentFactor > 7 → 钳到 7（协议违规，否则会算出荒谬的填充）；
///   * MaxTransferSize 小于一个满帧（hard_mtu）→ 反推并下调 MTU；
///     太小（<= 58，装不下头部）→ 报错；
///   * MaxTransferSize 报出 8KB/16KB 巨帧（WinCE / Windows Mobile 的习惯）→
///     不盲从，钳到我们自己的缓冲上限。
[[nodiscard]] Result<NegotiatedParameters> Negotiate(const InitializeComplete& complete,
                                                     std::uint32_t requested_mtu,
                                                     std::uint32_t host_transfer_size_limit);

// =============================================================================
// HALT
// =============================================================================

/// 编码 HALT_MSG。**设备不会回复**，发完即可认为进入 uninitialized。
[[nodiscard]] Result<std::uint32_t> EncodeHalt(std::uint32_t request_id,
                                               std::span<std::byte> buffer);

// =============================================================================
// QUERY / SET
// =============================================================================

struct QueryRequest {
  std::uint32_t request_id = 0;
  Oid oid = Oid::kGenSupportedList;
  /// 期望的响应字节数。
  ///
  /// 对**定长** OID 必须 ≥ 期望响应长度，并在消息尾部补上同样多的零字节；
  /// 对**变长** OID 必须为 0。这条未文档化的要求来自微软 ActiveSync 实现，
  /// 违反会得到 RNDIS_STATUS_INVALID_LENGTH。Encode 会按 IsVariableLengthOid()
  /// 自动纠正，调用方填 0 即可让它自己决定。
  std::uint32_t expected_response_bytes = 0;
};

/// 编码 QUERY_MSG（含按 OID 类型自动决定的尾部填充）。
[[nodiscard]] Result<std::uint32_t> Encode(const QueryRequest& request,
                                          std::span<std::byte> buffer);

struct QueryComplete {
  std::uint32_t request_id = 0;
  std::uint32_t status = 0;
  /// 指向 `buffer` 内部的信息缓冲区视图，生命周期同 `buffer`。
  std::span<const std::byte> information;
};

[[nodiscard]] Result<QueryComplete> DecodeQueryComplete(std::span<const std::byte> buffer);

struct SetRequest {
  std::uint32_t request_id = 0;
  Oid oid = Oid::kGenCurrentPacketFilter;
  std::span<const std::byte> information;
};

/// 编码 SET_MSG。
[[nodiscard]] Result<std::uint32_t> Encode(const SetRequest& request, std::span<std::byte> buffer);

/// 便捷入口：SET 一个 LE32 值（最常用，例如设置包过滤）。
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

/// 编码 RESET_MSG。
///
/// **注意：RESET_MSG 没有 RequestId 字段**（offset 8 是 Reserved），所以无法用
/// ID 配对响应，同一时刻只能有一个 RESET 在飞。
[[nodiscard]] Result<std::uint32_t> EncodeReset(std::span<std::byte> buffer);

struct ResetComplete {
  std::uint32_t status = 0;
  /// 非零表示寻址信息（包过滤、组播表）在复位中丢失，host 必须重发对应 SET。
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

/// 编码 KEEPALIVE_CMPLT。
///
/// 需要它是因为**设备也可以主动发 KEEPALIVE_MSG**，此时 host 必须回复
/// KEEPALIVE_CMPLT，否则设备可能认为 host 已死并断开。
[[nodiscard]] Result<std::uint32_t> EncodeKeepAliveComplete(std::uint32_t request_id,
                                                            std::uint32_t status,
                                                            std::span<std::byte> buffer);

// =============================================================================
// INDICATE_STATUS
// =============================================================================

struct IndicateStatus {
  std::uint32_t status = 0;
  /// 状态缓冲区视图；多数情况为空（MEDIA_CONNECT/DISCONNECT 不带负载）。
  std::span<const std::byte> status_buffer;
  /// 若状态缓冲区恰好是 8 字节的 RNDIS_Diagnostic_Info，这两个字段被填充。
  bool has_diagnostic_info = false;
  std::uint32_t diagnostic_status = 0;
  std::uint32_t diagnostic_error_offset = 0;
};

/// 解码 INDICATE_STATUS_MSG。
///
/// **StatusBufferOffset 的基准点在规范里是矛盾的**（MS 文档说消息起始，而同族
/// 的 QUERY/SET 都是消息起始 +8）。本实现按以下顺序容错：
///   1. 若长度为 0 —— 无负载，直接成功（绝大多数情况）；
///   2. 先按「基准点 = 消息起始 + 8」解释，落在消息范围内则采用；
///   3. 否则按「基准点 = 消息起始」解释；
///   4. 两种都越界 —— 丢弃状态缓冲区但**仍返回成功**，因为 status 本身有用，
///      不能因为一个可选负载解析不了就断开链路。
[[nodiscard]] Result<IndicateStatus> DecodeIndicateStatus(std::span<const std::byte> buffer);

// =============================================================================
// OID 负载解析
// =============================================================================

/// 从 QUERY_CMPLT 的信息缓冲区里取一个 LE32。
[[nodiscard]] Result<std::uint32_t> ParseUint32(std::span<const std::byte> information);

/// 从信息缓冲区里取一个计数器值。
///
/// 统计类 OID（OID_GEN_XMIT_OK 等）可能返回 4 或 8 字节，两者都必须接受。
[[nodiscard]] Result<std::uint64_t> ParseCounter(std::span<const std::byte> information);

/// 从信息缓冲区里取 6 字节 MAC。
[[nodiscard]] Result<MacAddress> ParseMac(std::span<const std::byte> information);

}  // namespace tetherkitnext::rndis
