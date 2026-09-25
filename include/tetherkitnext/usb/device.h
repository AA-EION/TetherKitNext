// RNDIS USB 设备的发现、声明与端点解析。
//
// ★ macOS 上能不能声明 RNDIS 接口？—— 能，而且不需要 root ★
//
//   这是本项目最关键的可行性前提，结论来自逐个检查 /System/Library/Extensions
//   下所有 CDC 家族 kext 的 IOKitPersonalities：
//
//     AppleUSBACMControl0 = {class 2, subclass 2, protocol 0}
//     AppleUSBACMControl1 = {class 2, subclass 2, protocol 1}
//     AppleUSBECMControl  = {class 2, subclass 6, protocol *}
//     AppleUSBNCMControl  = {class 2, subclass 13, protocol *}
//     AppleUSBWCMControl  = {class 2, subclass 8, protocol *}
//
//   RNDIS 的通信类接口是 {class 0x02, subclass 0x02, protocol **0xFF**} ——
//   protocol 既不是 0 也不是 1，**没有任何 personality 匹配**。Android 常用的
//   {0xE0, 0x01, 0x03} 变体更是连 class 0xE0 的 personality 都不存在。
//   也就是说 **macOS 内核根本没有 RNDIS 驱动**（这正是 HoRNDIS 这类第三方
//   kext 存在的原因，也正是本项目存在的理由）。
//
//   数据类接口 {0x0A, 0x00, 0x00} 确实会被 AppleUSBECMData0 / AppleUSBACMData0
//   probe 到，但它们的 start() 需要在同一设备上找到配对的 Control 驱动
//   （通过 CDC Union 描述符的 bMasterInterface）；RNDIS 场景下通信接口没被任何
//   Control 驱动接管，配对查找失败 → start() 返回 false → 驱动脱离，
//   该接口的 IORegistry 节点最终没有子节点，libusb 的
//   darwin_kernel_driver_active() 会返回 0。
//
//   设备级会有 AppleUSBCDCCompositeDevice 挂上（它的 IOProviderClass 是
//   IOUSBHostDevice，匹配面很宽），但它只做 ConfigureDevice / 发布 interface，
//   **不 open 任何 interface**，因此不影响 USBInterfaceOpen。
//
//   → 结论：普通非沙箱命令行程序**不需要 root、不需要 entitlement**，
//     libusb_open + libusb_claim_interface 即可成功。
//     （本项目整体仍需 root，但那是 feth 与 BPF 的要求，不是 libusb 的。）
//
// ★ 绝对不要开 libusb_set_auto_detach_kernel_driver ★
//
//   在 macOS 上它会让 claim 走 darwin_capture_claim_interface，一旦
//   darwin_kernel_driver_active 判为真就去「detach」—— 而 darwin 的 detach
//   实现是**重新枚举整个设备**（USBDeviceReEnumerate + CaptureDeviceMask），
//   且需要 com.apple.vm.device-access entitlement 或 root，否则返回
//   LIBUSB_ERROR_ACCESS。RNDIS 场景下本来没人占接口，开它纯属引入破坏性操作。
#pragma once

#include <libusb.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/rndis/protocol.h"
#include "tetherkitnext/usb/context.h"
#include "tetherkitnext/rndis/control_channel.h"

namespace tetherkitnext::usb {

/// 一个被识别为 RNDIS 的候选设备。
struct DeviceCandidate {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;

  /// 通信类接口（承载控制通道 + 中断通知）。
  std::uint8_t control_interface = 0;
  /// 数据类接口（承载 bulk IN/OUT）。
  std::uint8_t data_interface = 0;
  /// 匹配到的接口签名，用于日志说明「按哪种 RNDIS 形态识别的」。
  rndis::InterfaceSignature signature{};
  /// 是否走了 Android quirk 的兜底路径（见 DeviceFinder 的注释）。
  bool used_android_quirk = false;

  /// 形如 "Bus 020 Device 003: 18d1:4ee4"。
  [[nodiscard]] std::string Describe() const;
};

/// 设备筛选条件。全为 0 / 空表示不限。
struct DeviceFilter {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  /// 只匹配指定总线上的指定地址（两者必须同时给出）。
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;
};

/// 枚举当前连接的、看起来像 RNDIS 的设备。
///
/// 识别逻辑（顺序即优先级）：
///   1. 遍历所有配置的所有接口，找 signature 命中 kControlSignature* 的接口；
///   2. 排除伪 RNDIS：class == 0x02 且带非零 CDC ACM bmCapabilities 的是真
///      cdc-acm 调制解调器，不是 RNDIS。**这条检查只对 class 0x02 生效** ——
///      无线类（0xE0）的 RNDIS function 会把 bmCapabilities 挪作自用。
///   3. 找配对的数据接口：优先读 CDC Union 功能描述符的 bSlaveInterface；
///   4. **Android quirk**：许多 Android 设备的 CDC Union 描述符指向不存在的
///      接口号，或者干脆缺少 CDC 功能描述符。此时回落到「通信接口 = 0、
///      数据接口 = 1」的硬编码假设（Linux 的 android_rndis_quirk 就是这么做的），
///      并要求通信接口确实是 0。
[[nodiscard]] Result<std::vector<DeviceCandidate>> FindRndisDevices(const Context& context,
                                                                   const DeviceFilter& filter = {});

/// 已打开并声明好接口的 RNDIS 设备。
///
/// RAII：析构时按「释放接口 → 关闭句柄」的顺序拆除。
/// **调用方必须保证所有异步 transfer 在本对象析构前已经全部回收完毕** ——
/// libusb_close 不会帮你回收在飞 transfer（它只是把 transfer->dev_handle 置空
/// 并打一条 usbi_err），之后 IOKit 中止仍会让回调在 libusb 内部线程上跑，
/// 若那时 transfer 已被 free 就是 use-after-free。见 TransferPool 的拆除算法。
class Device {
 public:
  /// 打开设备并声明两个接口。
  [[nodiscard]] static Result<std::unique_ptr<Device>> Open(const Context& context,
                                                            const DeviceCandidate& candidate);

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  ~Device();

  [[nodiscard]] ::libusb_device_handle* Handle() const noexcept { return handle_; }

  [[nodiscard]] const DeviceCandidate& Candidate() const noexcept { return candidate_; }

  [[nodiscard]] std::string_view Describe() const noexcept { return description_; }

  // ---------------------------------------------------------------------------
  // 端点
  // ---------------------------------------------------------------------------

  /// bulk IN 端点地址（设备 → 主机）。
  [[nodiscard]] std::uint8_t BulkInEndpoint() const noexcept { return bulk_in_endpoint_; }

  /// bulk OUT 端点地址（主机 → 设备）。
  [[nodiscard]] std::uint8_t BulkOutEndpoint() const noexcept { return bulk_out_endpoint_; }

  /// 中断 IN 端点地址；0 表示设备没有中断端点（合法，需退化为轮询）。
  [[nodiscard]] std::uint8_t InterruptInEndpoint() const noexcept {
    return interrupt_in_endpoint_;
  }

  /// bulk 端点的 wMaxPacketSize。
  ///
  /// 用途有两处：① 推导 INITIALIZE_MSG 里该宣称的 MaxTransferSize；
  /// ② 判断 bulk OUT 传输长度是否恰为它的整数倍，从而决定要不要补 1 字节规避 ZLP。
  [[nodiscard]] std::uint16_t BulkMaxPacketSize() const noexcept { return bulk_max_packet_size_; }

  /// 中断端点的 wMaxPacketSize（RNDIS 通知固定 8 字节）。
  [[nodiscard]] std::uint16_t InterruptMaxPacketSize() const noexcept {
    return interrupt_max_packet_size_;
  }

  /// 设备的 USB 速度，用于日志与吞吐预期。
  [[nodiscard]] int Speed() const noexcept { return speed_; }

  [[nodiscard]] std::string_view SpeedName() const noexcept;

  /// 清除某个端点的 halt 状态。
  ///
  /// STALL 恢复可以在 transfer 回调里直接调用：libusb_clear_halt 走
  /// darwin_clear_halt → ClearPipeStallBothEnds，是一次同步 IOKit 调用，
  /// 不经过事件循环、没有 usbi_handling_events 守卫。代价是会阻塞事件线程
  /// 几十微秒到毫秒级，所以只在真的 STALL 时调。
  [[nodiscard]] Status ClearHalt(std::uint8_t endpoint);

 private:
  Device() = default;

  /// 从接口描述符里解析出三个端点。
  [[nodiscard]] Status ResolveEndpoints(const ::libusb_config_descriptor& config);

  ::libusb_device_handle* handle_ = nullptr;
  DeviceCandidate candidate_{};
  std::string description_;

  bool control_interface_claimed_ = false;
  bool data_interface_claimed_ = false;

  std::uint8_t bulk_in_endpoint_ = 0;
  std::uint8_t bulk_out_endpoint_ = 0;
  std::uint8_t interrupt_in_endpoint_ = 0;
  std::uint16_t bulk_max_packet_size_ = 0;
  std::uint16_t interrupt_max_packet_size_ = 0;
  int speed_ = 0;
};

/// 基于 libusb 的控制通道实现。
///
/// **必须从一个非 libusb 事件线程的专用线程上使用** —— 同步 API
/// （libusb_control_transfer）在事件线程上会返回 LIBUSB_ERROR_BUSY
/// （usbi_handling_events 是 TLS 判断）。
///
/// ★ 中断通知走**异步**传输，这不是优化而是必须 ★
///
///   若 WaitForNotification 改用同步 libusb_interrupt_transfer，
///   **控制线程会永久卡死**，栈是：
///       Poll → WaitForNotification → do_sync_bulk_transfer
///            → sync_transfer_wait_for_completion → handle_events → poll(∞)
///
///   原因链：
///     1. darwin 后端给所有 transfer 打 USBI_TRANSFER_OS_HANDLES_TIMEOUT，
///        表示「超时交给 IOKit 管，libusb 自己不计时」；于是
///        libusb_get_next_timeout 跳过它们、返回「无超时」，
///        sync_transfer_wait_for_completion 里的 poll() 无限期阻塞；
///     2. 而 IOKit 侧，darwin 的 submit_interrupt_transfer 用的是
///        **ReadPipeAsync（无超时变体）**，不是 bulk 用的 ReadPipeAsyncTO ——
///        **中断传输的 timeout 参数根本不被遵守**；
///     3. 于是端点上没有数据时，这个同步传输永远不会完成。
///
///   把 timeout 从 0 改成 1 ms 是**没用的**（第 2 条决定了它对中断端点无效）。
///   唯一的正确解法是：
///   提交一个常驻的**异步**中断传输，让它在 libusb 事件线程上完成，
///   WaitForNotification 只查一个原子标志，永不进入 libusb 的等待路径。
class UsbControlChannel final : public rndis::ControlChannel {
 public:
  UsbControlChannel(Device& device, std::uint32_t timeout_millis);

  UsbControlChannel(const UsbControlChannel&) = delete;
  UsbControlChannel& operator=(const UsbControlChannel&) = delete;
  UsbControlChannel(UsbControlChannel&&) = delete;
  UsbControlChannel& operator=(UsbControlChannel&&) = delete;

  ~UsbControlChannel() override;

  /// 提交常驻的异步中断传输。设备没有中断端点时是空操作。
  [[nodiscard]] Status StartNotificationListener();

  /// 取消中断传输并等回调回收完毕。**不能从 libusb 事件线程调用。**
  void StopNotificationListener();

  [[nodiscard]] Status SendMessage(std::span<const std::byte> message) override;
  [[nodiscard]] Result<std::span<const std::byte>> ReceiveMessage() override;
  [[nodiscard]] rndis::NotificationResult WaitForNotification(std::uint32_t timeout_millis) override;

  [[nodiscard]] std::uint32_t TimeoutMillis() const noexcept override { return timeout_millis_; }

  [[nodiscard]] std::string_view Describe() const noexcept override {
    return device_->Describe();
  }

 private:
  static void NotificationCallbackTrampoline(::libusb_transfer* transfer);
  void OnNotificationComplete() noexcept;

  Device* device_;
  std::uint32_t timeout_millis_;

  /// GET_ENCAPSULATED_RESPONSE 的接收缓冲。
  std::vector<std::byte> response_buffer_;
  /// 中断 IN 的通知缓冲（8 字节）。
  std::vector<std::byte> notification_buffer_;

  /// 常驻的异步中断传输。
  ::libusb_transfer* notification_transfer_ = nullptr;
  /// 设备是否通报了「有响应可取」。由事件线程置位，控制线程消费。
  std::atomic<bool> notification_pending_{false};
  /// 中断传输是否在飞。用于停机时等待回调回收（避免 use-after-free）。
  std::atomic<bool> notification_in_flight_{false};
  std::atomic<bool> notification_stopping_{false};
};

}  // namespace tetherkitnext::usb
