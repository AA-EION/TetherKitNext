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

/// The interface registration callback installed by the host. An atomic rather than a mutex: reads happen on every create/destroy,
/// and the destroy path is in a destructor and must be noexcept; taking a lock would introduce the possibility of throwing.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<InterfaceRegistry> g_interface_registry{nullptr};

/// Notifies the registration callback. A no-op when no callback is installed.
void NotifyRegistry(std::string_view name, bool created) noexcept {
  if (const InterfaceRegistry registry = g_interface_registry.load(std::memory_order_acquire);
      registry != nullptr) {
    registry(name, created);
  }
}

/// A temporary socket used only for issuing ioctls.
///
/// Interface-related ioctls need a socket as a handle; using AF_INET/SOCK_DGRAM is the convention
/// (ifconfig does the same), and no data is actually sent or received.
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

  /// Issues one ioctl; on failure wraps errno and the call name into an Error.
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

/// Fills an ifreq carrying only the interface name.
[[nodiscard]] Result<::ifreq> MakeIfreq(std::string_view name) {
  if (name.size() >= kInterfaceNameCapacity) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetInterfaceNameOverLimit, name, kInterfaceNameCapacity - 1)));
  }
  ::ifreq request{};
  std::memcpy(request.ifr_name, name.data(), name.size());
  return request;
}

/// Reads a 32-bit integer sysctl.
[[nodiscard]] Result<std::int32_t> ReadInt32Sysctl(const char* name) {
  std::int32_t value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetSysctlReadFailed, name)));
  }
  return value;
}

/// Issues one driver-private ioctl to feth.
[[nodiscard]] Status CallFethDriverIoctl(const IoctlSocket& socket, std::string_view interface_name,
                                        unsigned long ioctl_request, unsigned long command,
                                        FethRequest& payload, std::string_view what) {
  if (interface_name.size() >= kInterfaceNameCapacity) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetInterfaceNameTooLong, interface_name)));
  }

  IfDrv driver{};
  std::memcpy(driver.ifd_name, interface_name.data(), interface_name.size());
  driver.ifd_cmd = command;
  // The kernel checks ifd_len >= sizeof(struct if_fake_request); anything shorter gets EINVAL directly.
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
      // Some sysctls may not exist on particular macOS versions; a missing one is not an error, just log it.
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
  // feth_clone_destroy automatically unpairs the peer first internally, so we need not Unpeer first.
  if (const auto status = socket->Call(SIOCIFDESTROY, &*request, "ioctl(SIOCIFDESTROY)"); !status) {
    TETHERKITNEXT_ERROR_TR(Msg::kNetDestroyFailed, name, status.error().ToString());
    return;
  }
  NotifyRegistry(name, false);
  TETHERKITNEXT_INFO_TR(Msg::kNetDestroyed, name);
}

Result<FethInterface> FethInterface::Create(std::string_view requested_name) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const auto socket, IoctlSocket::Open());

  // Empty name -> fill in the driver name "feth" as a wildcard; the kernel picks the lowest free number and writes back the full name.
  const std::string_view name_to_request =
      requested_name.empty() ? std::string_view{kFethCloneName} : requested_name;
  TETHERKITNEXT_ASSIGN_OR_RETURN(::ifreq request, MakeIfreq(name_to_request));

  // SIOCIFCREATE and SIOCIFCREATE2 are fully equivalent for feth -- feth_clone_create ignores
  // params, and the only difference of SIOCIFCREATE2 is passing one extra params pointer. Use the simple one.
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

  // On wildcard creation the kernel writes the full name (including the number) back into ifr_name.
  std::string created_name(request.ifr_name,
                           ::strnlen(request.ifr_name, kInterfaceNameCapacity));
  // Register first, then return: the point of registering is "in case the process is forcibly killed from this moment on, the next run can still clean it up",
  // so no window may be left in between.
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

  // The reserved field is kept all zero -- the kernel checks it, and non-zero gets EINVAL directly.
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

  // An empty peer name (first byte '\0') means unpair.
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

  // feth_ioctl passes ifr_addr directly to ifnet_set_lladdr(ifp, sa_data, sa_len),
  // looking only at sa_len and sa_data, without verifying sa_family.
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
  // Reading the MAC goes through getifaddrs + AF_LINK rather than an ioctl: macOS's SDK only provides
  // SIOCSIFLLADDR (write), with no corresponding read ioctl. getifaddrs is the
  // standard way to read link addresses on BSD, and needs no extra privileges.
  ::ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kNetGetifaddrsFailed)));
  }
  // RAII guarantees the list is freed on any return path.
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

  // Read back the current flags first and then modify, to avoid clearing other flag bits.
  TETHERKITNEXT_RETURN_IF_ERROR(socket.Call(SIOCGIFFLAGS, &request, "ioctl(SIOCGIFFLAGS)"));

  // ifr_flags is a short, and IFF_* may exceed the range of short on 64-bit,
  // so uint16 mask arithmetic is used by macOS convention.
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

  // 1. Verify the sysctls that get snapshotted at creation -- **must be before creation**.
  TETHERKITNEXT_RETURN_IF_ERROR(VerifyFethSysctls());

  TETHERKITNEXT_ASSIGN_OR_RETURN(const std::uint32_t max_mtu, QueryFethMaxMtu());
  if (mtu > max_mtu) {
    return std::unexpected(Error::Generic(Tr(Msg::kNetMtuExceedsFethLimit, mtu, max_mtu)));
  }

  // 2. Create the two interfaces.
  TETHERKITNEXT_ASSIGN_OR_RETURN(FethInterface system_side, FethInterface::Create());
  TETHERKITNEXT_ASSIGN_OR_RETURN(FethInterface driver_side, FethInterface::Create());

  // 3. Pair -- done before UP, so that the link is up from the start.
  TETHERKITNEXT_RETURN_IF_ERROR(driver_side.PeerWith(system_side));

  // 4. The MTU of both sides must be the same, otherwise frames one side can send cannot be received by the other.
  TETHERKITNEXT_RETURN_IF_ERROR(system_side.SetMtu(mtu));
  TETHERKITNEXT_RETURN_IF_ERROR(driver_side.SetMtu(mtu));

  // 5. Set the system-side MAC -- **must be before UP**.
  //    The driver side deliberately keeps the kernel-assigned address: the MACs of the two sides must differ, otherwise the same IPv6 link-local
  //    address triggers a DAD conflict.
  if (system_mac != nullptr) {
    TETHERKITNEXT_RETURN_IF_ERROR(system_side.SetMacAddress(*system_mac));

    const auto driver_mac = driver_side.QueryMacAddress();
    if (driver_mac && *driver_mac == *system_mac) {
      return std::unexpected(
          Error::Generic(Tr(Msg::kNetSameMacDadConflict, FormatMac(*system_mac).data())));
    }
  }

  // 6. Bring both sides UP.
  //    bpfwrite has a hard check: if the interface is not IFF_UP it returns ENETDOWN.
  TETHERKITNEXT_RETURN_IF_ERROR(driver_side.SetUp(true));
  TETHERKITNEXT_RETURN_IF_ERROR(system_side.SetUp(true));

  TETHERKITNEXT_INFO_TR(Msg::kNetFethPairReady, system_side.Name(), driver_side.Name(), mtu);
  return FethPair{std::move(system_side), std::move(driver_side)};
}

}  // namespace tetherkitnext::net
