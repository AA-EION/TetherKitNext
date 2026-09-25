// C ABI 实现层的内部工具。**不对外安装**，只在 src/capi 内部使用。
//
// 这里集中处理「C++ 世界 ↔ C 世界」的三件重复劳动：把 std::string_view 拷进
// 定长缓冲、把 tetherkitnext::Error 翻译成 tk_error_t、取墙上时间。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/error.h"

namespace tetherkitnext::capi {

/// 把文本拷进定长缓冲并保证 NUL 结尾，超长时截断。
///
/// ★ 截断必须落在 UTF-8 的字符边界上 ★
///   本项目的错误消息全是中文，一个汉字 3 字节。按字节硬切会在缓冲末尾留下
///   半个字符，Swift 侧 `String(cString:)` 遇到非法序列会**整串变成替换字符**，
///   等于把整条错误信息毁掉。所以截断后要退回到最近的字符起点。
inline void CopyText(char* destination, std::size_t capacity, std::string_view text) noexcept {
  if (destination == nullptr || capacity == 0) {
    return;
  }
  std::size_t length = text.size() < capacity - 1 ? text.size() : capacity - 1;

  // length < text.size() 说明发生了截断：若切点落在了某个多字节序列的中间
  // （text[length] 是 0b10xxxxxx 的续接字节），就一路退到该序列的起点。
  while (length > 0 && length < text.size() &&
         (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U) {
    --length;
  }

  std::memcpy(destination, text.data(), length);
  destination[length] = '\0';
}

/// 数组版本，省掉每处都写 sizeof。
template <std::size_t N>
inline void CopyText(char (&destination)[N], std::string_view text) noexcept {
  CopyText(destination, N, text);
}

/// 把 C++ 侧的错误翻译成 C 结构体。`out_error` 可为 nullptr（调用方不关心原因）。
inline void FillError(tk_error_t* out_error, const Error& error) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = static_cast<std::int32_t>(error.Domain());
  out_error->code = error.Code();
  // 用 ToString() 而非 Context()：前者带上了「[libusb: LIBUSB_ERROR_ACCESS(-3)]」
  // 这样的域与码，用户报错时能直接贴给我们。
  CopyText(out_error->message, error.ToString());
}

/// 填一条没有底层错误码的纯逻辑错误。
inline void FillGenericError(tk_error_t* out_error, std::string_view message) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = TK_ERROR_DOMAIN_GENERIC;
  out_error->code = 0;
  CopyText(out_error->message, message);
}

/// 把错误结构体清成「无错误」。成功路径上调用，避免调用方读到上一次的残留。
inline void ClearError(tk_error_t* out_error) noexcept {
  if (out_error == nullptr) {
    return;
  }
  out_error->domain = TK_ERROR_DOMAIN_GENERIC;
  out_error->code = 0;
  out_error->message[0] = '\0';
}

/// 唯一确定「插在总线上的一台设备」的四元组。
///
/// 总线地址在拔插后可能被系统复用，多带上 VID:PID 才不会把一台设备的记忆
/// 错安到接替同一地址的另一台头上。
struct DeviceIdentity {
  std::uint8_t bus_number = 0;
  std::uint8_t device_address = 0;
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;

  [[nodiscard]] bool operator==(const DeviceIdentity&) const noexcept = default;
};

/// 从枚举结果里取设备身份。
[[nodiscard]] inline DeviceIdentity IdentityOf(const tk_device_info_t& info) noexcept {
  return DeviceIdentity{.bus_number = info.bus_number,
                        .device_address = info.device_address,
                        .vendor_id = info.vendor_id,
                        .product_id = info.product_id};
}

/// 一台设备最近一次**成功读到**的字符串描述符。
struct RememberedDeviceStrings {
  DeviceIdentity identity;
  char manufacturer[TK_USB_STRING_CAPACITY]{};
  char product[TK_USB_STRING_CAPACITY]{};
  char serial[TK_USB_STRING_CAPACITY]{};
};

/// 维护字符串描述符的记忆：读到了就记住，没读到就用记忆回填。
///
/// ★ 为什么需要 ★
///   字符串描述符要 libusb_open 才能读，而会话运行期间设备被本进程独占，
///   读取会被调用方跳过或直接失败 —— 但设备本身没变，「这一次拿不到」不等于
///   「设备没有名字」。不回填的话，连接瞬间的一次枚举就会把界面上的
///   「vivo iQOO Z10x」覆盖成「USB 设备 2d95:600b」，并在整个会话期间挂着。
///
/// 规则（`infos` 是本次枚举已填好基础字段的设备）：
///   - `infos[i]` 三个字符串**有任何一个非空** → 这次读到了，覆盖式记入 `memory`；
///   - 全空 → 没读到（被跳过或设备被占用），若 `memory` 里有同身份的记忆则回填；
///   - `memory` 里身份不在 `present`（当前总线上的全部设备）中的条目被清除 ——
///     设备已经拔掉，记忆过期；同一地址若再插上一台读不出名字的设备，
///     宁可显示 VID:PID 也不能把前一台的名字安给它。
///
/// `present` 可能比 `infos` 长：调用方给的数组容量不够时只填了前几台，
/// 后面那些仍然在场，它们的记忆不该被误删。
/// 纯逻辑、不碰 libusb，锁由调用方负责 —— 这样才能离线单测。
void ReconcileDeviceStrings(std::vector<RememberedDeviceStrings>& memory,
                            std::span<const DeviceIdentity> present,
                            std::span<tk_device_info_t> infos);

/// 自 Unix 纪元的墙上时间（纳秒）。
///
/// 事件与日志用墙上时间而非单调时间：它们要显示给人看，并且要能和 Console.app
/// 里的系统日志对齐。速率计算另有单调时间，见 tk_session_status.monotonic_nanos。
[[nodiscard]] std::int64_t WallNanos() noexcept;

/// 校验网卡名：只接受 `feth<数字>`。
///
/// 这不是洁癖 —— 网卡名会被拼进 `ipconfig` / `route` 的 argv。虽然我们用
/// posix_spawn 传数组而非过 shell（本身已经杜绝了注入），但把接口名限制成
/// 我们自己创建过的那种形态，还能挡住「误把 en0 传进来，把用户的 Wi-Fi 配置
/// 冲掉」这类更现实的事故。
[[nodiscard]] bool IsValidFethName(std::string_view name) noexcept;

/// 安装 feth 接口的落盘登记（见 orphan_cleanup.cc）。幂等。
///
/// 由 tk_session_create 调用 —— 只有真的要创建网卡时才需要登记，让免 root 的
/// 那组接口（版本、枚举、预检）保持零副作用。
void InstallInterfaceRegistry();

}  // namespace tetherkitnext::capi
