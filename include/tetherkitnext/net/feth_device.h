// feth（if_fake）虚拟网卡对的生命周期管理。
//
// 拓扑与数据流向（已对照 xnu 源码 feth_output_common() 确认）：
//
//     主机 IP 栈
//         │  配 IP / 路由 / DHCP
//         ▼
//     ┌────────┐   peer 配对   ┌────────┐      BPF
//     │ feth0  │ ◄───────────► │ feth1  │ ◄──────────► TetherKitNext
//     │(系统侧)│               │(驱动侧)│
//     └────────┘               └────────┘
//
//   * 主机从 feth0 发出的帧 → 在 feth1 上是 **input** 方向，被 BPF 抓到；
//   * 我们向 feth1 的 BPF **write** 一帧 → 走 feth1 的 output 路径 →
//     进入 feth0 的 input → 被主机 IP 栈收到。**不会** loopback 回 feth1。
//
// 所以 BPF 只需要挂在 feth1 上，一个描述符同时完成收和发。
//
// 权限：本文件所有操作都需要 root（SIOCIFCREATE / SIOCSDRVSPEC / SIOCSIFLLADDR /
// SIOCSIFFLAGS / SIOCSIFMTU 在内核里都有 proc_suser 检查）。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::net {

/// 6 字节 MAC 地址。
using MacAddress = std::array<std::uint8_t, 6>;

/// feth 接口名的最大长度（IFNAMSIZ = 16，含终止符）。
inline constexpr std::size_t kInterfaceNameCapacity = 16;

/// feth 支持的 MTU 上限，由 sysctl net.link.fake.max_mtu 决定（本机 2048）。
///
/// 注意该 sysctl 也是**创建期快照**：feth_max_mtu 在 clone_create 时算定，
/// 之后改 sysctl 对已存在的接口无效。
[[nodiscard]] Result<std::uint32_t> QueryFethMaxMtu();

/// 校验 feth 创建期会快照的那批 sysctl 是否都处于我们要求的值。
///
/// **必须在创建 feth 之前调用。** 这些开关（hwcsum / fcs / tso_support / lro /
/// trailer_length / separate_frame_header）一旦在创建时被快照成非 0，我们从
/// BPF 读到的就不是干净的以太帧，而创建后再改 sysctl 已经来不及了。
[[nodiscard]] Status VerifyFethSysctls();

/// 接口创建 / 销毁的登记回调。
///
/// ★ 为什么需要它 ★
///   进程被 SIGKILL 时 C++ 的析构不会跑，feth 会留在内核里；而 SIGKILL 是任何
///   信号处理器都拦不住的。唯一可靠的兜底是**把创建过的接口名落盘，下次启动时
///   清理**。RAII 管不了被强杀的进程，这个钩子就是为此存在的。
///
/// 约束：会在创建 / 销毁的同一线程上同步调用，实现里只该做「往文件里加一行 /
/// 删一行」这种短操作，不要打日志（销毁路径可能在析构里，noexcept）。
///
/// @param name    接口名。
/// @param created true = 刚创建，false = 刚销毁。
using InterfaceRegistry = void (*)(std::string_view name, bool created) noexcept;

/// 安装 / 卸载登记回调。传 nullptr 卸载。进程级，线程安全。
void SetInterfaceRegistry(InterfaceRegistry registry) noexcept;

/// 销毁一张按名字指定的 feth 接口。用于清理进程被强杀后残留的孤儿。
///
/// 与 FethInterface 的析构不同，这里不需要先持有对象 —— 孤儿正是「没有对象在
/// 管」的那些。名字必须形如 `feth<数字>`，调用方负责校验。
[[nodiscard]] Status DestroyInterfaceByName(std::string_view name);

/// 一张 feth 接口。RAII：析构时销毁内核里的接口。
class FethInterface {
 public:
  FethInterface() = default;

  FethInterface(const FethInterface&) = delete;
  FethInterface& operator=(const FethInterface&) = delete;
  FethInterface(FethInterface&& other) noexcept;
  FethInterface& operator=(FethInterface&& other) noexcept;

  /// 销毁接口。内核的 feth_clone_destroy 会自动先解绑 peer。
  ~FethInterface();

  /// 创建一张新的 feth 接口。
  ///
  /// @param requested_name 传空串表示让内核自动选最小空闲编号（推荐）；
  ///                       传 "feth7" 表示指定编号，已存在则失败（EEXIST）。
  ///
  /// 内核在通配模式下会把完整名字写回，可用 Name() 取得。
  [[nodiscard]] static Result<FethInterface> Create(std::string_view requested_name = {});

  [[nodiscard]] bool Valid() const noexcept { return !name_.empty(); }

  [[nodiscard]] std::string_view Name() const noexcept { return name_; }

  /// 与另一张 feth 接口配对。
  ///
  /// 内核对此有五项硬性校验，任一不满足返回 EINVAL：
  ///   1. ifd_len >= sizeof(if_fake_request)（160）；
  ///   2. if_fake_request 的 reserved 字段必须全零；
  ///   3. peer 必须也是 feth（ifnet_name() == "feth" 且 type == IFT_ETHER）；
  ///   4. 双方都不能已有 peer；
  ///   5. 调用者必须是 root。
  [[nodiscard]] Status PeerWith(const FethInterface& peer);

  /// 解绑 peer（写入空的 peer 名）。
  [[nodiscard]] Status Unpeer();

  /// 查询当前 peer 名；未配对时返回空串。
  [[nodiscard]] Result<std::string> QueryPeer() const;

  /// 设置 MTU。上限由 QueryFethMaxMtu() 给出，超限返回 EINVAL。
  [[nodiscard]] Status SetMtu(std::uint32_t mtu);

  [[nodiscard]] Result<std::uint32_t> QueryMtu() const;

  /// 设置 MAC 地址。**必须在置 IFF_UP 之前调用。**
  ///
  /// 内核默认分配的 MAC 是 'f','e','t','h', unit>>8, unit&0xff
  /// （所以 feth0 是 66:65:74:68:00:00）。
  ///
  /// 对 RNDIS 场景应把**系统侧**那张（feth0）的 MAC 设为设备通过
  /// OID_802_3_PERMANENT_ADDRESS 汇报的地址 —— RNDIS 语义下设备就是这块网卡，
  /// 对端的 ARP 表与 DHCP 租约都按这个 MAC 建立。
  ///
  /// **驱动侧那张（feth1）必须保留内核分配的、与 feth0 不同的 MAC**，
  /// 否则两侧的 IPv6 链路本地地址相同，会触发 DAD 地址冲突。
  [[nodiscard]] Status SetMacAddress(const MacAddress& mac);

  [[nodiscard]] Result<MacAddress> QueryMacAddress() const;

  /// 置 IFF_UP / 清 IFF_UP。内核在置 UP 时会自动补上 IFF_RUNNING。
  [[nodiscard]] Status SetUp(bool up);

  [[nodiscard]] Result<bool> IsUp() const;

  /// 放弃对接口的所有权，析构时不再销毁它（用于排障时保留现场）。
  void Release() noexcept { name_.clear(); }

 private:
  explicit FethInterface(std::string name) : name_(std::move(name)) {}

  /// 销毁内核里的接口并清空 name_。失败只记日志 —— 析构路径不能抛也不能返回错误。
  void Destroy() noexcept;

  std::string name_;
};

/// 配置完成、可以投入使用的 feth 接口对。
class FethPair {
 public:
  /// 创建并配置一对 feth。
  ///
  /// 操作顺序是刻意安排的（顺序错了会失败或产生错误状态）：
  ///   1. 校验创建期 sysctl —— 必须在创建之前；
  ///   2. 创建两张接口；
  ///   3. 配对 —— 在 UP 之前配对，这样链路一开始就是 up 的
  ///      （SIOCGIFMEDIA 的 IFM_ACTIVE 立刻为真）；
  ///   4. 设 MTU；
  ///   5. 设系统侧 MAC —— 必须在 UP 之前；
  ///   6. 两侧都置 UP —— bpfwrite 硬性要求 feth1 是 UP（否则 ENETDOWN），
  ///      feth0 也必须 UP 才能让主机 IP 栈处理收到的帧。
  ///
  /// @param mtu           两侧的 MTU。
  /// @param system_mac    系统侧（host_side）要设置的 MAC；传 std::nullopt
  ///                      表示保留内核分配的地址。
  [[nodiscard]] static Result<FethPair> Create(std::uint32_t mtu,
                                               const MacAddress* system_mac = nullptr);

  /// 系统侧接口：主机在这张网卡上配 IP、跑 DHCP、加路由。
  [[nodiscard]] const FethInterface& SystemSide() const noexcept { return system_side_; }

  /// 驱动侧接口：TetherKitNext 把 BPF 挂在这张上收发原始帧。
  [[nodiscard]] const FethInterface& DriverSide() const noexcept { return driver_side_; }

  [[nodiscard]] FethInterface& SystemSide() noexcept { return system_side_; }

  [[nodiscard]] FethInterface& DriverSide() noexcept { return driver_side_; }

 private:
  FethPair(FethInterface system_side, FethInterface driver_side)
      : system_side_(std::move(system_side)), driver_side_(std::move(driver_side)) {}

  FethInterface system_side_;
  FethInterface driver_side_;
};

/// 把 MAC 渲染成 "aa:bb:cc:dd:ee:ff"。
[[nodiscard]] std::array<char, 18> FormatMac(const MacAddress& mac) noexcept;

/// 当前进程是否以 root 运行。用于在启动时给出明确的报错而非一串 EPERM。
[[nodiscard]] bool IsRunningAsRoot() noexcept;

}  // namespace tetherkitnext::net
