// 免 root 的三件事：版本、环境预检、设备枚举。
//
// GUI 本体（uid 501）只调这一组；需要 root 的会话与网卡配置全部交给
// tetherkitnext-helper。这个划分是 docs/GUI-SPIKE.md 的核心结论之一。
#include <libusb.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "capi_support.h"
#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/net/feth_device.h"
#include "tetherkitnext/usb/context.h"
#include "tetherkitnext/usb/device.h"
#include "tetherkitnext/version.h"

namespace {

using tetherkitnext::capi::ClearError;
using tetherkitnext::capi::CopyText;
using tetherkitnext::capi::FillError;

/// 读一个 USB 字符串描述符到定长缓冲。索引为 0 表示设备没提供该字符串。
///
/// 用 libusb 的 `_ascii` 变体：它把 UTF-16LE 里超出 ASCII 的码位替换成 '?'。
/// 对我们够用 —— 这些字符串只用于让用户区分两台同型号设备，绝大多数厂商名与
/// 序列号本来就是 ASCII。自己做完整的 UTF-16 → UTF-8 转换收益太小。
void ReadStringDescriptor(::libusb_device_handle* handle, std::uint8_t index, char* destination,
                          std::size_t capacity) noexcept {
  if (index == 0) {
    return;
  }
  std::array<unsigned char, TK_USB_STRING_CAPACITY> buffer{};
  const int length = ::libusb_get_string_descriptor_ascii(
      handle, index, buffer.data(), static_cast<int>(buffer.size()));
  if (length <= 0) {
    return;
  }
  CopyText(destination, capacity,
           std::string_view{reinterpret_cast<const char*>(buffer.data()),
                            static_cast<std::size_t>(length)});
}

/// 尽力而为地补上厂商名 / 产品名 / 序列号。
///
/// 为什么是「尽力而为」：读字符串描述符必须先 libusb_open，而设备可能已被本机
/// 另一个进程（比如正在跑的 tetherkitnext-helper）独占，darwin 后端会返回
/// LIBUSB_ERROR_ACCESS。那不是错误，只是拿不到名字 —— 此时由字符串记忆回填
/// 上次读到的值（见 tk_list_devices 末尾），连记忆都没有才回落到 VID:PID。
/// 绝不能因此让整个枚举失败。
void TryReadStrings(::libusb_context* context, tk_device_info_t& info) noexcept {
  ::libusb_device** list = nullptr;
  const ssize_t count = ::libusb_get_device_list(context, &list);
  if (count < 0) {
    return;
  }

  for (ssize_t i = 0; i < count; ++i) {
    if (::libusb_get_bus_number(list[i]) != info.bus_number ||
        ::libusb_get_device_address(list[i]) != info.device_address) {
      continue;
    }

    ::libusb_device_descriptor descriptor{};
    if (::libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS) {
      break;
    }
    ::libusb_device_handle* handle = nullptr;
    if (::libusb_open(list[i], &handle) != LIBUSB_SUCCESS) {
      break;
    }
    ReadStringDescriptor(handle, descriptor.iManufacturer, info.manufacturer,
                         sizeof(info.manufacturer));
    ReadStringDescriptor(handle, descriptor.iProduct, info.product, sizeof(info.product));
    ReadStringDescriptor(handle, descriptor.iSerialNumber, info.serial, sizeof(info.serial));
    ::libusb_close(handle);
    break;
  }

  // 第二个参数为 1：连同 list 里每个设备的引用计数一起释放。
  ::libusb_free_device_list(list, 1);
}

/// 进程级共享的 libusb 上下文，**仅供枚举使用**。
///
/// ★ 为什么必须共享 ★
///   每次枚举都新建一个上下文，等于每次都走一遍「初始化 libusb → 起事件线程
///   → 起 IOKit runloop 线程 → 用完全部拆掉」。GUI 每几百毫秒刷新一次设备
///   列表，这套开销就以同样的频率重复，日志里也会被「libusb 已初始化」刷满。
///
///   共享之后整个进程只初始化一次。长命上下文照样能看到新插上的设备 ——
///   libusb 的 darwin 后端有自己的热插拔线程在维护设备列表，这正是所有
///   libusb 程序依赖的常规机制。
///
/// 会话（core::Runtime）另有自己的上下文，不复用这一个：它的生命周期由会话
/// 自己管，混进来只会让停机顺序变复杂。libusb 支持同进程多上下文。
///
/// 初始化失败时返回 nullptr 并填错误，且**不缓存失败**，下次调用会重试。
tetherkitnext::usb::Context* SharedEnumerationContext(tk_error_t* out_error) {
  static std::mutex mutex;
  static std::unique_ptr<tetherkitnext::usb::Context> context;

  const std::lock_guard<std::mutex> guard(mutex);
  if (context == nullptr) {
    auto created = tetherkitnext::usb::Context::Create();
    if (!created) {
      FillError(out_error, created.error());
      return nullptr;
    }
    context = std::move(*created);
  }
  return context.get();
}

/// 字符串描述符的进程级记忆（锁 + 条目），与共享枚举上下文同款的函数局部 static。
///
/// 放在进程级而不是调用方：helper 是常驻进程，GUI 中途重启后第一次枚举
/// 就能拿到回填的名字；纯逻辑见 ReconcileDeviceStrings 的说明。
struct StringMemory {
  std::mutex mutex;
  std::vector<tetherkitnext::capi::RememberedDeviceStrings> entries;
};

StringMemory& SharedStringMemory() {
  static StringMemory memory;
  return memory;
}

void FillDeviceInfo(const tetherkitnext::usb::DeviceCandidate& candidate,
                    tk_device_info_t& info) noexcept {
  info = tk_device_info_t{};
  info.vendor_id = candidate.vendor_id;
  info.product_id = candidate.product_id;
  info.bus_number = candidate.bus_number;
  info.device_address = candidate.device_address;
  info.control_interface = candidate.control_interface;
  info.data_interface = candidate.data_interface;
  info.interface_class = candidate.signature.interface_class;
  info.interface_subclass = candidate.signature.interface_subclass;
  info.interface_protocol = candidate.signature.interface_protocol;
  info.used_android_quirk = candidate.used_android_quirk;
  CopyText(info.description, candidate.Describe());
}

}  // namespace

namespace {

/// tk_language_t → C++ 侧的 Language。越界返回 nullopt，让调用方忽略这次设置。
[[nodiscard]] std::optional<tetherkitnext::Language> ToLanguage(std::int32_t value) noexcept {
  switch (value) {
    case TK_LANGUAGE_ENGLISH:
      return tetherkitnext::Language::kEnglish;
    case TK_LANGUAGE_CHINESE:
      return tetherkitnext::Language::kChinese;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::int32_t FromLanguage(tetherkitnext::Language language) noexcept {
  return language == tetherkitnext::Language::kChinese ? TK_LANGUAGE_CHINESE : TK_LANGUAGE_ENGLISH;
}

}  // namespace

void tk_set_language(int32_t language) {
  if (const std::optional<tetherkitnext::Language> parsed = ToLanguage(language);
      parsed.has_value()) {
    tetherkitnext::SetLanguage(*parsed);
  }
}

int32_t tk_get_language(void) {
  return FromLanguage(tetherkitnext::GetLanguage());
}

int32_t tk_detect_system_language(void) {
  return FromLanguage(tetherkitnext::DetectLanguageFromEnvironment());
}

void tk_version(tk_version_info_t* out_version) {
  if (out_version == nullptr) {
    return;
  }
  *out_version = tk_version_info_t{};
  const tetherkitnext::Version version = tetherkitnext::GetVersion();
  out_version->major = version.major;
  out_version->minor = version.minor;
  out_version->patch = version.patch;
  CopyText(out_version->text, tetherkitnext::GetVersionString());
  CopyText(out_version->build, tetherkitnext::GetBuildDescription());
  CopyText(out_version->libusb, tetherkitnext::usb::Context::VersionString());
}

tk_result_t tk_check_environment(tk_environment_t* out_environment) {
  if (out_environment == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }
  *out_environment = tk_environment_t{};

  out_environment->is_root = tetherkitnext::net::IsRunningAsRoot();

  // 这批 sysctl 是 feth 的**创建期快照**，创建后再改无效，所以必须提前查。
  // 不合格时把原因原样交给 GUI 展示 —— 用户看到具体是哪个开关被打开了，
  // 才知道该改什么。
  if (const auto status = tetherkitnext::net::VerifyFethSysctls(); status) {
    out_environment->sysctls_ok = true;
  } else {
    out_environment->sysctls_ok = false;
    CopyText(out_environment->sysctl_detail, status.error().ToString());
  }

  if (const auto max_mtu = tetherkitnext::net::QueryFethMaxMtu(); max_mtu) {
    out_environment->feth_max_mtu = *max_mtu;
  }

  // 刻意永远返回成功：「环境不合格」是要展示给用户的**结果**，不是调用失败。
  return TK_OK;
}

tk_result_t tk_list_devices(tk_device_info_t* out_devices, size_t capacity, size_t* out_count,
                            bool read_strings, tk_error_t* out_error) {
  if (out_count == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }
  ClearError(out_error);
  *out_count = 0;

  tetherkitnext::usb::Context* context = SharedEnumerationContext(out_error);
  if (context == nullptr) {
    return TK_ERR_FAILED;
  }

  auto candidates = tetherkitnext::usb::FindRndisDevices(*context, {});
  if (!candidates) {
    FillError(out_error, candidates.error());
    return TK_ERR_FAILED;
  }

  *out_count = candidates->size();
  if (out_devices == nullptr) {
    return TK_OK;
  }

  const std::size_t writable = std::min(capacity, candidates->size());
  for (std::size_t i = 0; i < writable; ++i) {
    FillDeviceInfo((*candidates)[i], out_devices[i]);
    if (read_strings) {
      TryReadStrings(context->Raw(), out_devices[i]);
    }
  }

  // 读到就记住、没读到就回填 —— 会话运行期间（read_strings=false 或设备被占用）
  // 名字才不会从界面上消失。present 用完整候选集而非 writable 前缀：
  // 容量不够时后面那些设备仍然在场，它们的记忆不能被当作「已拔掉」清除。
  {
    std::vector<tetherkitnext::capi::DeviceIdentity> present;
    present.reserve(candidates->size());
    for (const tetherkitnext::usb::DeviceCandidate& candidate : *candidates) {
      present.push_back({.bus_number = candidate.bus_number,
                         .device_address = candidate.device_address,
                         .vendor_id = candidate.vendor_id,
                         .product_id = candidate.product_id});
    }
    StringMemory& memory = SharedStringMemory();
    const std::lock_guard<std::mutex> guard(memory.mutex);
    tetherkitnext::capi::ReconcileDeviceStrings(memory.entries, present, {out_devices, writable});
  }
  return TK_OK;
}
