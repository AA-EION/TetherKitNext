#include "tetherkitnext/net/feth_device.h"

#include <ifaddrs.h>
#include <net/if_dl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <utility>

#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/net/darwin_abi.h"

namespace tetherkitnext::net {
namespace {

/// 宿主安装的接口登记回调。用原子而非互斥锁：读发生在每次创建/销毁，
/// 而销毁路径在析构里、必须 noexcept，加锁会引入抛异常的可能。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<InterfaceRegistry> g_interface_registry{nullptr};

/// 通知登记回调。回调未安装时是空操作。
void NotifyRegistry(std::string_view name, bool created) noexcept {
  if (const InterfaceRegistry registry = g_interface_registry.load(std::memory_order_acquire);
      registry != nullptr) {
    registry(name, created);
  }
}

/// 一个只用来发 ioctl 的临时套接字。
///
/// 接口相关的 ioctl 需要一个 socket 作为句柄；用 AF_INET/SOCK_DGRAM 是惯例
/// （ifconfig 也是这么做的），并不真的收发数据。
class IoctlSocket {
 public:
  [[nodiscard]] static Result<IoctlSocket> Open() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetIoctlSocketFailed)));
    }
    return IoctlSocket{fd};
  }

  IoctlSocket(const IoctlSocket&) = delete;
  IoctlSocket& operator=(const IoctlSocket&) = delete;

  IoctlSocket(IoctlSocket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  IoctlSocket& operator=(IoctlSocket&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  ~IoctlSocket() { Close(); }

  [[nodiscard]] int Fd() const noexcept { return fd_; }

  /// 发一次 ioctl，失败时把 errno 与调用名包进 Error。
  [[nodiscard]] Status Call(unsigned long request, void* argument, std::string_view what) const {
    if (::ioctl(fd_, request, argument) < 0) {
      return std::unexpected(Error::FromErrno(0, std::string{what}));
    }
    return Ok();
  }

 private:
  explicit IoctlSocket(int fd) : fd_(fd) {}

  void Close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd_ = -1;
};

/// 填一个只带接口名的 ifreq。
[[nodiscard]] Result<::ifreq> MakeIfreq(std::string_view name) {
  if (name.size() >= kInterfaceNameCapacity) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetInterfaceNameOverLimit, name, kInterfaceNameCapacity - 1)));
  }
  ::ifreq request{};
  std::memcpy(request.ifr_name, name.data(), name.size());
  return request;
}

/// 读一个 32 位整型 sysctl。
[[nodiscard]] Result<std::int32_t> ReadInt32Sysctl(const char* name) {
  std::int32_t value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetSysctlReadFailed, name)));
  }
  return value;
}

/// 对 feth 发一次驱动私有 ioctl。
[[nodiscard]] Status CallFethDriverIoctl(const IoctlSocket& socket, std::string_view interface_name,
                                        unsigned long ioctl_request, unsigned long command,
                                        FethRequest& payload, std::string_view what) {
  if (interface_name.size() >= kInterfaceNameCapacity) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetInterfaceNameTooLong, interface_name)));
  }

  IfDrv driver{};
  std::memcpy(driver.ifd_name, interface_name.data(), interface_name.size());
  driver.ifd_cmd = command;
  // 内核校验 ifd_len >= sizeof(struct if_fake_request)，短了直接 EINVAL。
  driver.ifd_len = sizeof(payload);
  driver.ifd_data = &payload;

  return socket.Call(ioctl_request, &driver, what);
}

}  // namespace

std::array<char, 18> FormatMac(const MacAddress& mac) noexcept {
  std::array<char, 18> text{};
  std::snprintf(text.data(), text.size(), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
  return text;
}

bool IsRunningAsRoot() noexcept {
  return ::geteuid() == 0;
}

Result<std::uint32_t> QueryFethMaxMtu() {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const std::int32_t value, ReadInt32Sysctl("net.link.fake.max_mtu"));
  if (value <= 0) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetFethMaxMtuInvalid, value)));
  }
  return static_cast<std::uint32_t>(value);
}

Status VerifyFethSysctls() {
  for (const RequiredFethSysctl& entry : kRequiredFethSysctls) {
    const auto value = ReadInt32Sysctl(entry.name);
    if (!value) {
      // 某些 sysctl 在特定 macOS 版本上可能不存在；缺失不算错误，只记一条。
      TETHERKITNEXT_DEBUG_TR(Msg::kNetSysctlUnreadableSkipped, entry.name,
                         value.error().ToString());
      continue;
    }
    if (*value != entry.required_value) {
      return std::unexpected(Error::Generic(Tr(Msg::kNetSysctlMismatch, entry.name, *value,
                                               entry.required_value, Text(entry.why))));
    }
  }
  return Ok();
}

void SetInterfaceRegistry(InterfaceRegistry registry) noexcept {
  g_interface_registry.store(registry, std::memory_order_release);
}

Status DestroyInterfaceByName(std::string_view name) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name));

  if (const auto status = socket.Call(SIOCIFDESTROY, &request, "ioctl(SIOCIFDESTROY)"); !status) {
    Error error = status.error();
    return std::unexpected(
        std::move(error).WithContext(Tr(Msg::kNetDestroyOrphanFailed, name)));
  }
  NotifyRegistry(name, false);
  TETHERKITNEXT_INFO_TR(Msg::kNetDestroyedOrphan, name);
  return Ok();
}

// =============================================================================
// FethInterface
// =============================================================================

FethInterface::FethInterface(FethInterface&& other) noexcept
    : name_(std::exchange(other.name_, {})) {}

FethInterface& FethInterface::operator=(FethInterface&& other) noexcept {
  if (this != &other) {
    Destroy();
    name_ = std::exchange(other.name_, {});
  }
  return *this;
}

FethInterface::~FethInterface() {
  Destroy();
}

void FethInterface::Destroy() noexcept {
  if (name_.empty()) {
    return;
  }
  const std::string name = std::exchange(name_, {});

  const auto socket = IoctlSocket::Open();
  if (!socket) {
    TETHERKITNEXT_ERROR_TR(Msg::kNetDestroyFailed, name, socket.error().ToString());
    return;
  }
  auto request = MakeIfreq(name);
  if (!request) {
    TETHERKITNEXT_ERROR_TR(Msg::kNetDestroyFailed, name, request.error().ToString());
    return;
  }
  // feth_clone_destroy 内部会自动先解绑 peer，无需我们先 Unpeer。
  if (const auto status = socket->Call(SIOCIFDESTROY, &*request, "ioctl(SIOCIFDESTROY)"); !status) {
    TETHERKITNEXT_ERROR_TR(Msg::kNetDestroyFailed, name, status.error().ToString());
    return;
  }
  NotifyRegistry(name, false);
  TETHERKITNEXT_INFO_TR(Msg::kNetDestroyed, name);
}

Result<FethInterface> FethInterface::Create(std::string_view requested_name) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());

  // 名字为空 → 填驱动名 "feth" 作为通配，内核选最小空闲编号并写回完整名字。
  const std::string_view name_to_request =
      requested_name.empty() ? std::string_view{kFethCloneName} : requested_name;
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name_to_request));

  // SIOCIFCREATE 与 SIOCIFCREATE2 对 feth 完全等价 —— feth_clone_create 忽略
  // params，而 SIOCIFCREATE2 唯一的区别就是多传一个 params 指针。用简单的那个。
  if (const auto status = socket.Call(SIOCIFCREATE, &request, "ioctl(SIOCIFCREATE)"); !status) {
    Error error = status.error();
    if (error.Code() == EPERM) {
      return std::unexpected(
          std::move(error).WithContext(Tr(Msg::kNetFethCreateNeedsRoot)));
    }
    if (error.Code() == EEXIST) {
      return std::unexpected(
          std::move(error).WithContext(Tr(Msg::kNetInterfaceExists, name_to_request)));
    }
    return std::unexpected(
        std::move(error).WithContext(Tr(Msg::kNetFethCreateFailed, name_to_request)));
  }

  // 通配创建时内核把完整名字（含编号）写回 ifr_name。
  std::string created_name(request.ifr_name,
                           ::strnlen(request.ifr_name, kInterfaceNameCapacity));
  // 先登记再返回：登记的意义就是「万一从这一刻起进程被强杀，下次也能清掉它」，
  // 所以中间不能留任何窗口。
  NotifyRegistry(created_name, true);
  TETHERKITNEXT_INFO_TR(Msg::kNetCreated, created_name);
  return FethInterface{std::move(created_name)};
}

Status FethInterface::PeerWith(const FethInterface& peer) {
  if (!Valid() || !peer.Valid()) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetPeerInvalidObject)));
  }
  if (peer.Name().size() >= kInterfaceNameCapacity) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetPeerNameTooLong, peer.Name())));
  }

  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());

  // reserved 字段保持全零 —— 内核会校验，非零直接 EINVAL。
  FethRequest payload{};
  std::memcpy(payload.u.peer_name, peer.Name().data(), peer.Name().size());

  if (const auto status =
          CallFethDriverIoctl(socket, name_, kSetDriverSpec,
                              static_cast<unsigned long>(FethSetCommand::kSetPeer), payload,
                              "ioctl(SIOCSDRVSPEC, IF_FAKE_S_CMD_SET_PEER)");
      !status) {
    Error error = status.error();
    if (error.Code() == EPERM) {
      return std::unexpected(std::move(error).WithContext(Tr(Msg::kNetPeerNeedsRoot)));
    }
    if (error.Code() == EINVAL) {
      return std::unexpected(std::move(error).WithContext(
          Tr(Msg::kNetPeerRejected, name_, peer.Name())));
    }
    return std::unexpected(
        std::move(error).WithContext(Tr(Msg::kNetPeerFailed, name_, peer.Name())));
  }

  TETHERKITNEXT_INFO_TR(Msg::kNetPeered, name_, peer.Name());
  return Ok();
}

Status FethInterface::Unpeer() {
  if (!Valid()) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetUnpeerInvalidObject)));
  }
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());

  // 空 peer 名（首字节 '\0'）即表示解绑。
  FethRequest payload{};
  TETHERKITNEXT_RETURN_IF_ERROR(CallFethDriverIoctl(
      socket, name_, kSetDriverSpec, static_cast<unsigned long>(FethSetCommand::kSetPeer), payload,
      Tr(Msg::kNetUnpeerIoctlWhat)));
  return Ok();
}

Result<std::string> FethInterface::QueryPeer() const {
  if (!Valid()) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetQueryPeerInvalidObject)));
  }
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());

  FethRequest payload{};
  TETHERKITNEXT_RETURN_IF_ERROR(CallFethDriverIoctl(
      socket, name_, kGetDriverSpec, static_cast<unsigned long>(FethGetCommand::kGetPeer), payload,
      "ioctl(SIOCGDRVSPEC, IF_FAKE_G_CMD_GET_PEER)"));

  return std::string(payload.u.peer_name,
                     ::strnlen(payload.u.peer_name, kInterfaceNameCapacity));
}

Status FethInterface::SetMtu(std::uint32_t mtu) {
  if (!Valid()) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetSetMtuInvalidObject)));
  }
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name_));
  request.ifr_mtu = static_cast<int>(mtu);

  if (const auto status = socket.Call(SIOCSIFMTU, &request, "ioctl(SIOCSIFMTU)"); !status) {
    Error error = status.error();
    if (error.Code() == EINVAL) {
      const auto limit = QueryFethMaxMtu();
      return std::unexpected(std::move(error).WithContext(
          Tr(Msg::kNetSetMtuRejected, name_, mtu,
             limit ? std::format("{}", *limit) : std::string{Text(Msg::kNetUnknownValue)})));
    }
    return std::unexpected(
        std::move(error).WithContext(Tr(Msg::kNetSetMtuFailed, name_, mtu)));
  }
  return Ok();
}

Result<std::uint32_t> FethInterface::QueryMtu() const {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name_));
  TETHERKITNEXT_RETURN_IF_ERROR(socket.Call(SIOCGIFMTU, &request, "ioctl(SIOCGIFMTU)"));
  return static_cast<std::uint32_t>(request.ifr_mtu);
}

Status FethInterface::SetMacAddress(const MacAddress& mac) {
  if (!Valid()) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetSetMacInvalidObject)));
  }
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name_));

  // feth_ioctl 直接把 ifr_addr 转给 ifnet_set_lladdr(ifp, sa_data, sa_len)，
  // 只看 sa_len 与 sa_data，不校验 sa_family。
  request.ifr_addr.sa_len = static_cast<std::uint8_t>(mac.size());
  request.ifr_addr.sa_family = AF_LINK;
  std::memcpy(request.ifr_addr.sa_data, mac.data(), mac.size());

  if (const auto status = socket.Call(SIOCSIFLLADDR, &request, "ioctl(SIOCSIFLLADDR)"); !status) {
    Error error = status.error();
    if (error.Code() == EPERM) {
      return std::unexpected(std::move(error).WithContext(Tr(Msg::kNetSetMacNeedsRoot)));
    }
    return std::unexpected(std::move(error).WithContext(
        Tr(Msg::kNetSetMacFailed, name_, FormatMac(mac).data())));
  }
  TETHERKITNEXT_INFO_TR(Msg::kNetMacSet, name_, FormatMac(mac).data());
  return Ok();
}

Result<MacAddress> FethInterface::QueryMacAddress() const {
  // 读 MAC 走 getifaddrs + AF_LINK 而非 ioctl：macOS 的 SDK 只提供
  // SIOCSIFLLADDR（写），没有对应的读 ioctl。getifaddrs 是 BSD 上读链路地址的
  // 标准做法，且不需要额外权限。
  ::ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetGetifaddrsFailed)));
  }
  // 用 RAII 保证任何返回路径都释放链表。
  const std::unique_ptr<::ifaddrs, decltype(&::freeifaddrs)> guard(list, &::freeifaddrs);

  for (const ::ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_LINK) {
      continue;
    }
    if (name_ != entry->ifa_name) {
      continue;
    }
    const auto* link = reinterpret_cast<const ::sockaddr_dl*>(entry->ifa_addr);
    if (link->sdl_alen != sizeof(MacAddress)) {
      return std::unexpected(
          Error::Generic(Tr(Msg::kNetLinkAddressWrongLength, name_, link->sdl_alen)));
    }
    MacAddress mac{};
    std::memcpy(mac.data(), LLADDR(link), mac.size());
    return mac;
  }
  return std::unexpected(Error::Generic(Tr(Msg::kNetLinkAddressNotFound, name_)));
}

Status FethInterface::SetUp(bool up) {
  if (!Valid()) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetSetUpInvalidObject)));
  }
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name_));

  // 先读回当前 flags 再改，避免把别的标志位清掉。
  TETHERKITNEXT_RETURN_IF_ERROR(socket.Call(SIOCGIFFLAGS, &request, "ioctl(SIOCGIFFLAGS)"));

  // ifr_flags 是 short，而 IFF_* 在 64 位下可能超出 short 范围，
  // 因此按 macOS 的惯例用 uint16 掩码运算。
  auto flags = static_cast<std::uint16_t>(request.ifr_flags);
  if (up) {
    flags |= static_cast<std::uint16_t>(IFF_UP);
  } else {
    flags &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(IFF_UP));
  }
  request.ifr_flags = static_cast<short>(flags);

  if (const auto status = socket.Call(SIOCSIFFLAGS, &request, "ioctl(SIOCSIFFLAGS)"); !status) {
    Error error = status.error();
    if (error.Code() == EPERM) {
      return std::unexpected(std::move(error).WithContext(Tr(Msg::kNetSetUpNeedsRoot)));
    }
    return std::unexpected(
        std::move(error).WithContext(Tr(Msg::kNetSetUpFailed, name_, up ? "UP" : "DOWN")));
  }
  return Ok();
}

Result<bool> FethInterface::IsUp() const {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name_));
  TETHERKITNEXT_RETURN_IF_ERROR(socket.Call(SIOCGIFFLAGS, &request, "ioctl(SIOCGIFFLAGS)"));
  return (static_cast<std::uint16_t>(request.ifr_flags) & static_cast<std::uint16_t>(IFF_UP)) != 0;
}

// =============================================================================
// FethPair
// =============================================================================

Result<FethPair> FethPair::Create(std::uint32_t mtu, const MacAddress* system_mac) {
  if (!IsRunningAsRoot()) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetFethPairNeedsRoot)));
  }

  // 1. 校验创建期会被快照的 sysctl —— **必须在创建之前**。
  TETHERKITNEXT_RETURN_IF_ERROR(VerifyFethSysctls());

  TETHERKITNEXT_ASSIGN_OR_RETURN(const std::uint32_t max_mtu, QueryFethMaxMtu());
  if (mtu > max_mtu) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetMtuExceedsFethLimit, mtu, max_mtu)));
  }

  // 2. 创建两张接口。
  TETHERKITNEXT_ASSIGN_OR_RETURN(FethInterface system_side, FethInterface::Create());
  TETHERKITNEXT_ASSIGN_OR_RETURN(FethInterface driver_side, FethInterface::Create());

  // 3. 配对 —— 在 UP 之前做，这样链路从一开始就是 up 的。
  TETHERKITNEXT_RETURN_IF_ERROR(driver_side.PeerWith(system_side));

  // 4. 两侧 MTU 必须一致，否则一侧能发的帧另一侧收不下。
  TETHERKITNEXT_RETURN_IF_ERROR(system_side.SetMtu(mtu));
  TETHERKITNEXT_RETURN_IF_ERROR(driver_side.SetMtu(mtu));

  // 5. 设系统侧 MAC —— **必须在 UP 之前**。
  //    驱动侧刻意保留内核分配的地址：两侧 MAC 必须不同，否则 IPv6 链路本地
  //    地址相同会触发 DAD 冲突。
  if (system_mac != nullptr) {
    TETHERKITNEXT_RETURN_IF_ERROR(system_side.SetMacAddress(*system_mac));

    const auto driver_mac = driver_side.QueryMacAddress();
    if (driver_mac && *driver_mac == *system_mac) {
      return std::unexpected(
          Error::Generic(Tr(Msg::kNetSameMacDadConflict, FormatMac(*system_mac).data())));
    }
  }

  // 6. 两侧都置 UP。
  //    bpfwrite 里有硬检查：接口不是 IFF_UP 就返回 ENETDOWN。
  TETHERKITNEXT_RETURN_IF_ERROR(driver_side.SetUp(true));
  TETHERKITNEXT_RETURN_IF_ERROR(system_side.SetUp(true));

  TETHERKITNEXT_INFO_TR(Msg::kNetFethPairReady, system_side.Name(), driver_side.Name(), mtu);
  return FethPair{std::move(system_side), std::move(driver_side)};
}

}  // namespace tetherkitnext::net
