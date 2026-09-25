// RNDIS 控制通道抽象。
//
// 为什么这个接口放在 rndis 层而不是 usb 层：它描述的是**RNDIS 协议**的控制通道
// 语义（发一条消息、取一条消息、等一个「有响应了」的通知），而不是 USB 的语义。
// 状态机（tk_rndis）依赖它，libusb 实现（tk_usb）提供它 —— 这样依赖方向就是
// 正确的 rndis ← usb，而不会出现 tk_rndis 反向依赖 tk_usb。
//
// RNDIS 的控制消息不走 bulk 端点，而是走 USB 控制端点（EP0）的两个类请求：
//   SEND_ENCAPSULATED_COMMAND  (bmRequestType=0x21, bRequest=0x00)
//   GET_ENCAPSULATED_RESPONSE  (bmRequestType=0xA1, bRequest=0x01)
// 外加通信类接口上的中断 IN 端点，用来通知「有响应可取了」。
//
// 为什么要抽象成接口：状态机是整个 RNDIS 实现里逻辑最复杂、最需要测试的部分，
// 而开发机上没有任何 USB 设备。把控制通道抽象掉之后，状态机可以完全在内存里
// 被驱动，包括那些真机上极难复现的路径（设备主动发 KEEPALIVE、
// INDICATE_STATUS 插队、RESET 后要求重放、响应乱序、超时重试）。
//
// 性能上这里用虚函数完全没问题：控制通道每 5 秒才有一次保活往返，
// 而且 Apple Silicon 上 libusb 的控制传输本身就是**毫秒**级
// （libusb issue #1288：M2 上 control transfer 比 x64 慢约 10 倍）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::rndis {

/// 「只探一下、不要真等」时应传的超时值。
///
/// ⚠️ **不能传 0。** libusb 在 darwin 上把 timeout 同时作为 noDataTimeout 与
/// completionTimeout 传给 IOKit，而 **0 表示无限等待** —— 传 0 会让调用线程
/// 永久阻塞在 `libusb_wait_for_event` 上，整个控制循环卡死、连停机信号都响应不了。
/// （详见 AGENTS.md 第 7 节第 12、13 条。）
/// libusb 的同步 API 没有真正的非阻塞模式，能做到的最短等待就是 1 ms。
inline constexpr std::uint32_t kProbeOnlyTimeoutMillis = 1;

/// 等待通知的结果。
enum class NotificationResult : std::uint8_t {
  kResponseAvailable,  ///< 设备明确通知有响应可取。
  kTimeout,            ///< 等待超时。**不一定是错误** —— 见下方说明。
  kNotSupported,       ///< 设备没有中断端点，调用方应直接轮询。
};

/// RNDIS 控制通道。
///
/// 实现必须保证：所有方法都从**同一个线程**调用（状态机线程）。这不是为了
/// 简化实现，而是 libusb 的硬性约束 —— 同步 API 从事件线程调用会返回
/// LIBUSB_ERROR_BUSY，所以控制通道必须独占一个非事件线程。
class ControlChannel {
 public:
  ControlChannel() = default;
  ControlChannel(const ControlChannel&) = delete;
  ControlChannel& operator=(const ControlChannel&) = delete;
  ControlChannel(ControlChannel&&) = delete;
  ControlChannel& operator=(ControlChannel&&) = delete;
  virtual ~ControlChannel() = default;

  /// 发送一条 RNDIS 控制消息（SEND_ENCAPSULATED_COMMAND）。
  [[nodiscard]] virtual Status SendMessage(std::span<const std::byte> message) = 0;

  /// 取回一条 RNDIS 控制消息（GET_ENCAPSULATED_RESPONSE）。
  ///
  /// 返回的视图指向实现内部的缓冲，在下一次调用本方法之前有效。
  ///
  /// **返回空视图不是错误**：规范规定设备在尚无有效响应时应返回**1 字节 0x00**
  /// 而不是 STALL 控制端点。因此调用方收到长度 < 8 字节的结果必须当作
  /// 「还没准备好，稍后重试」，而不是当作失败。
  [[nodiscard]] virtual Result<std::span<const std::byte>> ReceiveMessage() = 0;

  /// 等待设备的 RESPONSE_AVAILABLE 通知。
  ///
  /// 中断端点的处理必须同时兼容两类设备行为：
  ///   * 规范做法：host 等中断 IN 上的 8 字节通知
  ///     （两个 LE32：0x00000001 = RESPONSE_AVAILABLE，0）；
  ///   * Linux 做法：完全忽略中断端点，直接对 GET_ENCAPSULATED_RESPONSE
  ///     轮询最多 10 次、每次间隔 40 ms。
  /// 而且反过来还存在**某些设备必须先被中断端点读过一次才会在控制端点上作答**。
  /// 所以正确策略是：先等通知，超时后仍然去轮询一次。kTimeout 因此不是错误。
  ///
  /// @param timeout_millis 等待上限。**传 0 表示无限等待**（与 libusb 的语义
  ///        一致）—— 想「只探一下」请传 kProbeOnlyTimeoutMillis，不要传 0。
  ///        实现应对 0 做防御性钳位，见 UsbControlChannel。
  [[nodiscard]] virtual NotificationResult WaitForNotification(std::uint32_t timeout_millis) = 0;

  /// 控制传输的超时（毫秒）。
  [[nodiscard]] virtual std::uint32_t TimeoutMillis() const noexcept = 0;

  /// 供日志使用的可读标识（如 "Bus 020 Device 003: 18d1:4ee4"）。
  [[nodiscard]] virtual std::string_view Describe() const noexcept = 0;
};

}  // namespace tetherkitnext::rndis
