// Lifecycle management of an feth (if_fake) virtual NIC pair.
//
// Topology and data flow (confirmed against the xnu source feth_output_common()):
//
//     Host IP stack
//         │  configure IP / routes / DHCP
//         ▼
//     ┌────────┐  peer pairing ┌────────┐      BPF
//     │ feth0  │ ◄───────────► │ feth1  │ ◄──────────► TetherKitNext
//     │(system)│               │(driver)│
//     └────────┘               └────────┘
//
//   * Frames the host sends out of feth0 -> are in the **input** direction on feth1 and are captured by BPF;
//   * When we **write** a frame to feth1's BPF -> it goes through feth1's output path ->
//     enters feth0's input -> and is received by the host IP stack. It **does not** loop back to feth1.
//
// So BPF only needs to attach to feth1, and one descriptor handles both receiving and sending.
//
// Privileges: all operations in this file need root (SIOCIFCREATE / SIOCSDRVSPEC / SIOCSIFLLADDR /
// SIOCSIFFLAGS / SIOCSIFMTU all have proc_suser checks in the kernel).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "tetherkitnext/common/error.h"

namespace tetherkitnext::net {

/// 6-byte MAC address.
using MacAddress = std::array<std::uint8_t, 6>;

/// Maximum length of an feth interface name (IFNAMSIZ = 16, including the terminator).
inline constexpr std::size_t kInterfaceNameCapacity = 16;

/// Upper limit of MTU supported by feth, determined by the sysctl net.link.fake.max_mtu (2048 on this machine).
///
/// Note that this sysctl is also a **creation-time snapshot**: feth_max_mtu is fixed at clone_create,
/// and changing the sysctl afterwards has no effect on existing interfaces.
[[nodiscard]] Result<std::uint32_t> QueryFethMaxMtu();

/// Verifies whether the sysctls snapshotted at feth creation are all at the values we require.
///
/// **Must be called before creating the feth.** Once these switches (hwcsum / fcs / tso_support / lro /
/// trailer_length / separate_frame_header) are snapshotted as non-zero at creation, what we read from
/// BPF is not a clean Ethernet frame, and changing the sysctl after creation is too late.
[[nodiscard]] Status VerifyFethSysctls();

/// Registration callback for interface creation / destruction.
///
/// * Why it is needed *
///   When a process is SIGKILLed, C++ destructors do not run and the feth stays in the kernel; and SIGKILL cannot be intercepted by any
///   signal handler. The only reliable backstop is to **persist the names of created interfaces to disk and
///   clean up on the next launch**. RAII cannot manage a forcibly killed process; this hook exists for exactly that.
///
/// Constraint: it is called synchronously on the same thread as creation / destruction; the implementation should only do short operations like "append a line to a file /
/// delete a line", and must not log (the destruction path may be in a destructor, noexcept).
///
/// @param name    Interface name.
/// @param created true = just created, false = just destroyed.
using InterfaceRegistry = void (*)(std::string_view name, bool created) noexcept;

/// Installs / uninstalls the registration callback. Pass nullptr to uninstall. Process-wide, thread-safe.
void SetInterfaceRegistry(InterfaceRegistry registry) noexcept;

/// Destroys an feth interface specified by name. Used to clean up orphans left after a process was forcibly killed.
///
/// Unlike the destructor of FethInterface, no object needs to be held first here -- orphans are exactly those "with no object
/// managing them". The name must be of the form `feth<number>`; the caller is responsible for validation.
[[nodiscard]] Status DestroyInterfaceByName(std::string_view name);

/// An feth interface. RAII: destroys the kernel interface on destruction.
class FethInterface {
 public:
  FethInterface() = default;

  FethInterface(const FethInterface&) = delete;
  FethInterface& operator=(const FethInterface&) = delete;
  FethInterface(FethInterface&& other) noexcept;
  FethInterface& operator=(FethInterface&& other) noexcept;

  /// Destroys the interface. The kernel's feth_clone_destroy automatically unpairs the peer first.
  ~FethInterface();

  /// Creates a new feth interface.
  ///
  /// @param requested_name An empty string lets the kernel automatically choose the lowest free number (recommended);
  ///                       "feth7" specifies the number, and fails if it already exists (EEXIST).
  ///
  /// In wildcard mode the kernel writes back the full name, which can be obtained with Name().
  [[nodiscard]] static Result<FethInterface> Create(std::string_view requested_name = {});

  [[nodiscard]] bool Valid() const noexcept { return !name_.empty(); }

  [[nodiscard]] std::string_view Name() const noexcept { return name_; }

  /// Pairs with another feth interface.
  ///
  /// The kernel has five hard checks for this; failing any returns EINVAL:
  ///   1. ifd_len >= sizeof(if_fake_request) (160);
  ///   2. the reserved field of if_fake_request must be all zero;
  ///   3. the peer must also be an feth (ifnet_name() == "feth" and type == IFT_ETHER);
  ///   4. neither side may already have a peer;
  ///   5. the caller must be root.
  [[nodiscard]] Status PeerWith(const FethInterface& peer);

  /// Unpairs the peer (writes an empty peer name).
  [[nodiscard]] Status Unpeer();

  /// Queries the current peer name; returns an empty string when unpaired.
  [[nodiscard]] Result<std::string> QueryPeer() const;

  /// Sets the MTU. The upper limit is given by QueryFethMaxMtu(); exceeding it returns EINVAL.
  [[nodiscard]] Status SetMtu(std::uint32_t mtu);

  [[nodiscard]] Result<std::uint32_t> QueryMtu() const;

  /// Sets the MAC address. **Must be called before setting IFF_UP.**
  ///
  /// The MAC the kernel assigns by default is 'f','e','t','h', unit>>8, unit&0xff
  /// (so feth0 is 66:65:74:68:00:00).
  ///
  /// For the RNDIS scenario, the MAC of the **system-side** one (feth0) should be set to the address the device reports via
  /// OID_802_3_PERMANENT_ADDRESS -- under RNDIS semantics the device is this NIC,
  /// and the peer's ARP table and DHCP lease are both built on this MAC.
  ///
  /// **The driver-side one (feth1) must keep the kernel-assigned MAC, different from feth0's**,
  /// otherwise the IPv6 link-local addresses on both sides are the same, triggering a DAD address conflict.
  [[nodiscard]] Status SetMacAddress(const MacAddress& mac);

  [[nodiscard]] Result<MacAddress> QueryMacAddress() const;

  /// Sets IFF_UP / clears IFF_UP. When setting UP, the kernel automatically adds IFF_RUNNING.
  [[nodiscard]] Status SetUp(bool up);

  [[nodiscard]] Result<bool> IsUp() const;

  /// Gives up ownership of the interface; it will no longer be destroyed on destruction (to preserve the scene when troubleshooting).
  void Release() noexcept { name_.clear(); }

 private:
  explicit FethInterface(std::string name) : name_(std::move(name)) {}

  /// Destroys the kernel interface and clears name_. Failures are only logged -- the destructor path can neither throw nor return an error.
  void Destroy() noexcept;

  std::string name_;
};

/// An feth interface pair that is configured and ready for use.
class FethPair {
 public:
  /// Creates and configures a pair of feth.
  ///
  /// The order of operations is deliberate (a wrong order fails or produces a wrong state):
  ///   1. verify creation-time sysctls -- must be before creation;
  ///   2. create the two interfaces;
  ///   3. pair -- pair before UP, so that the link is up from the start
  ///      (IFM_ACTIVE of SIOCGIFMEDIA is immediately true);
  ///   4. set MTU;
  ///   5. set the system-side MAC -- must be before UP;
  ///   6. bring both sides UP -- bpfwrite strictly requires feth1 to be UP (otherwise ENETDOWN),
  ///      and feth0 must also be UP for the host IP stack to process received frames.
  ///
  /// @param mtu           MTU of both sides.
  /// @param system_mac    The MAC to set on the system side (host_side); passing std::nullopt
  ///                      means keeping the kernel-assigned address.
  [[nodiscard]] static Result<FethPair> Create(std::uint32_t mtu,
                                               const MacAddress* system_mac = nullptr);

  /// System-side interface: the host configures IP, runs DHCP and adds routes on this NIC.
  [[nodiscard]] const FethInterface& SystemSide() const noexcept { return system_side_; }

  /// Driver-side interface: TetherKitNext attaches BPF to this one to send and receive raw frames.
  [[nodiscard]] const FethInterface& DriverSide() const noexcept { return driver_side_; }

  [[nodiscard]] FethInterface& SystemSide() noexcept { return system_side_; }

  [[nodiscard]] FethInterface& DriverSide() noexcept { return driver_side_; }

 private:
  FethPair(FethInterface system_side, FethInterface driver_side)
      : system_side_(std::move(system_side)), driver_side_(std::move(driver_side)) {}

  FethInterface system_side_;
  FethInterface driver_side_;
};

/// Renders a MAC as "aa:bb:cc:dd:ee:ff".
[[nodiscard]] std::array<char, 18> FormatMac(const MacAddress& mac) noexcept;

/// Whether the current process runs as root. Used to give a clear error at startup instead of a string of EPERM.
[[nodiscard]] bool IsRunningAsRoot() noexcept;

}  // namespace tetherkitnext::net
