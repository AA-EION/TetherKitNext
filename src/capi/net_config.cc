// Configuration of the virtual NIC's connectivity method: DHCP / static IP / revoke, plus readback of the actual in-effect state.
//
// * Why DHCP establishes a real macOS network service *
//
//   `ipconfig set` only establishes a temporary service under State:/Network/Service. Ordinary traffic can use it,
//   but when NetworkExtension takes over routing it does not treat such a service as a reliable underlying path: a VPN
//   provider gets "No network route" even though feth0 still has an address and a scoped route.
//
//   DHCP mode therefore registers feth in the current network set via SCPreferences / SCNetworkService.
//   feth has no IOKit node, so public APIs cannot enumerate it; managed_network_service.cc uses only
//   the BSD-name constructor SPI that Apple itself exports to fill in the SCNetworkInterface, and the subsequent service,
//   protocol, commit and apply all go through public APIs. The service is removed when the feth is destroyed.
//
// * The DNS of a static IP is "best effort + readback verification" *
//
//   IPConfiguration publishes DNS only in DHCP mode (it comes from DHCP options).
//   MANUAL mode has no DNS source, so all we can do is add a DNS key to the service it established.
//   This move is **not verified on real hardware**, so success is never promised to upper layers: tk_net_query always reads back
//   the resolvers actually in effect in the system, and what the GUI shows is the readback rather than the value we applied.
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "capi_support.h"
#include "core_foundation_support.h"
#include "managed_network_service.h"
#include "process_runner.h"
#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"

namespace {

using tetherkitnext::Error;
using tetherkitnext::Msg;
using tetherkitnext::Result;
using tetherkitnext::Status;
using tetherkitnext::Text;
using tetherkitnext::Tr;
using tetherkitnext::capi::ClearError;
using tetherkitnext::capi::CopyText;
using tetherkitnext::capi::CopyToStdString;
using tetherkitnext::capi::FillError;
using tetherkitnext::capi::FillGenericError;
using tetherkitnext::capi::IsValidFethName;
using tetherkitnext::capi::MakeCFString;
using tetherkitnext::capi::ProcessResult;
using tetherkitnext::capi::RunTool;
using tetherkitnext::capi::ScopedCFRef;
using tetherkitnext::capi::SharedDynamicStore;

constexpr std::string_view kIpconfigPath = "/usr/sbin/ipconfig";
constexpr std::string_view kRoutePath = "/sbin/route";

/// Upper limit of waiting for a DHCP lease.
///
/// Basis for 10 seconds: DHCP DISCOVER retransmits use exponential backoff (1/2/4/8 seconds), and 10 seconds is enough to cover
/// the first three retransmissions. Any longer and we should tell the user "there is probably no DHCP server on the other side" instead of spinning on.
constexpr std::chrono::seconds kDhcpLeaseTimeout{10};
constexpr std::chrono::milliseconds kDhcpPollInterval{250};
/// How many consecutive times the gateway must read the same value before the lease is considered stable.
///
/// 8 times x 250 ms = 2 seconds. Basis: Android's USB tethering first hands out a 464XLAT transitional lease
/// (192.0.0.2 / gateway 192.0.0.1), and measurement shows it is replaced by the real subnet within 1 second; a 2-second quiet period
/// is enough to span that switch without making "right the first time" devices wait too long for nothing.
constexpr int kDhcpStableSamples = 8;

// ---------------------------------------------------------------------------
// Parameter validation
// ---------------------------------------------------------------------------

[[nodiscard]] bool IsValidIpv4(std::string_view text) noexcept {
  if (text.empty() || text.size() >= TK_ADDRESS_CAPACITY) {
    return false;
  }
  const std::string owned{text};
  ::in_addr parsed{};
  return ::inet_pton(AF_INET, owned.c_str(), &parsed) == 1;
}

/// Validates the interface name and translates it into a plain-language error.
[[nodiscard]] Status ValidateInterface(const char* interface_name) {
  if (interface_name == nullptr) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiInterfaceNameNull)));
  }
  if (!IsValidFethName(interface_name)) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiInterfaceNotOurs, interface_name)));
  }
  return tetherkitnext::Ok();
}

// ---------------------------------------------------------------------------
// External tool invocation
// ---------------------------------------------------------------------------

/// Runs a tool command; a non-zero exit is treated as failure and its output is carried along as-is.
///
/// Carrying it as-is matters: the error output of ipconfig / route is itself the most accurate diagnostic information,
/// and relaying it a second time ourselves would only lose information.
[[nodiscard]] Status RunOrFail(std::string_view executable,
                               const std::vector<std::string>& arguments,
                               std::string_view what) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const ProcessResult result, RunTool(executable, arguments));
  if (!result.Succeeded()) {
    std::string detail{result.output};
    // Tool output often has a trailing newline, which looks ugly when spliced into a single-line error.
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
      detail.pop_back();
    }
    return std::unexpected(Error::Generic(
        Tr(Msg::kCapiCommandFailed, what, result.exit_code,
           detail.empty() ? std::string{Text(Msg::kCapiCommandNoOutput)} : detail)));
  }
  return tetherkitnext::Ok();
}

/// Reads the interface's current IPv4 address; returns std::nullopt when there is no address.
///
/// Uses ioctl rather than `ipconfig getifaddr`: this is the real state in the kernel, going through no
/// intermediate layer, and needs no forked process.
[[nodiscard]] std::optional<std::string> QueryAddress(std::string_view interface_name,
                                                      unsigned long request) noexcept {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return std::nullopt;
  }

  ::ifreq ifr{};
  const std::size_t copy_length = std::min(interface_name.size(), sizeof(ifr.ifr_name) - 1);
  std::memcpy(ifr.ifr_name, interface_name.data(), copy_length);
  ifr.ifr_addr.sa_family = AF_INET;

  std::optional<std::string> result;
  if (::ioctl(fd, request, &ifr) == 0) {
    std::array<char, INET_ADDRSTRLEN> text{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* address = reinterpret_cast<const ::sockaddr_in*>(&ifr.ifr_addr);
    if (::inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) != nullptr) {
      result = std::string{text.data()};
    }
  }
  ::close(fd);
  return result;
}

// ---------------------------------------------------------------------------
// SCDynamicStore queries
// ---------------------------------------------------------------------------

/// Finds the service ID that IPConfiguration established for this interface.
///
/// The approach is to enumerate `State:/Network/Service/<id>/IPv4` and match by `InterfaceName`,
/// rather than **guessing** how the service ID is spelled. Measured, the ID obtained from `ipconfig set feth0 DHCP` is
/// "DHCP-feth0", which looks like it could be assembled directly, but that is an implementation detail, and matching by field is what is reliable.
[[nodiscard]] std::optional<std::string> FindServiceId(SCDynamicStoreRef store,
                                                       std::string_view interface_name) {
  const ScopedCFRef<CFStringRef> pattern = MakeCFString("State:/Network/Service/[^/]+/IPv4");
  const ScopedCFRef<CFArrayRef> keys{
      ::SCDynamicStoreCopyKeyList(store, pattern.Get())};
  if (!keys) {
    return std::nullopt;
  }

  const CFIndex count = ::CFArrayGetCount(keys.Get());
  for (CFIndex i = 0; i < count; ++i) {
    const auto* const key = static_cast<CFStringRef>(::CFArrayGetValueAtIndex(keys.Get(), i));
    const ScopedCFRef<CFDictionaryRef> entry{
        static_cast<CFDictionaryRef>(::SCDynamicStoreCopyValue(store, key))};
    if (!entry) {
      continue;
    }
    const auto* const name = static_cast<CFStringRef>(
        ::CFDictionaryGetValue(entry.Get(), CFSTR("InterfaceName")));
    if (name == nullptr || CopyToStdString(name) != interface_name) {
      continue;
    }

    // Cut <id> out of "State:/Network/Service/<id>/IPv4".
    const std::string full = CopyToStdString(key);
    constexpr std::string_view kPrefix = "State:/Network/Service/";
    constexpr std::string_view kSuffix = "/IPv4";
    if (full.size() <= kPrefix.size() + kSuffix.size()) {
      continue;
    }
    return full.substr(kPrefix.size(), full.size() - kPrefix.size() - kSuffix.size());
  }
  return std::nullopt;
}

[[nodiscard]] ScopedCFRef<CFDictionaryRef> CopyServiceEntry(SCDynamicStoreRef store,
                                                            std::string_view service_id,
                                                            std::string_view leaf) {
  const ScopedCFRef<CFStringRef> key =
      MakeCFString(std::format("State:/Network/Service/{}/{}", service_id, leaf));
  return ScopedCFRef<CFDictionaryRef>{
      static_cast<CFDictionaryRef>(::SCDynamicStoreCopyValue(store, key.Get()))};
}

/// Gets a string field from a dictionary; returns an empty string when missing.
[[nodiscard]] std::string StringField(CFDictionaryRef dictionary, CFStringRef field) {
  if (dictionary == nullptr) {
    return {};
  }
  const auto* const value = static_cast<CFStringRef>(::CFDictionaryGetValue(dictionary, field));
  if (value == nullptr || ::CFGetTypeID(value) != ::CFStringGetTypeID()) {
    return {};
  }
  return CopyToStdString(value);
}

/// Gets a string-array field from a dictionary.
[[nodiscard]] std::vector<std::string> StringArrayField(CFDictionaryRef dictionary,
                                                        CFStringRef field) {
  std::vector<std::string> values;
  if (dictionary == nullptr) {
    return values;
  }
  const auto* const array = static_cast<CFArrayRef>(::CFDictionaryGetValue(dictionary, field));
  if (array == nullptr || ::CFGetTypeID(array) != ::CFArrayGetTypeID()) {
    return values;
  }
  const CFIndex count = ::CFArrayGetCount(array);
  values.reserve(static_cast<std::size_t>(count));
  for (CFIndex i = 0; i < count; ++i) {
    const auto* const item = static_cast<CFStringRef>(::CFArrayGetValueAtIndex(array, i));
    if (item != nullptr && ::CFGetTypeID(item) == ::CFStringGetTypeID()) {
      values.push_back(CopyToStdString(item));
    }
  }
  return values;
}

/// Which interface the global default route currently points to.
[[nodiscard]] std::string PrimaryInterface(SCDynamicStoreRef store) {
  const ScopedCFRef<CFStringRef> key = MakeCFString("State:/Network/Global/IPv4");
  const ScopedCFRef<CFDictionaryRef> global{
      static_cast<CFDictionaryRef>(::SCDynamicStoreCopyValue(store, key.Get()))};
  return StringField(global.Get(), CFSTR("PrimaryInterface"));
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

/// Writes DNS servers onto the service that IPConfiguration established.
///
/// WARNING: **A move not verified on real hardware.** What is confirmed is that hand-**fabricating** a whole service is not adopted by IPMonitor
/// (GUI-SPIKE section 3.2). The bet here is that "adding a DNS key onto a service that is **already properly registered**
/// will be adopted". Losing the bet is not destructive, DNS just does not take effect -- so failures are only
/// logged and never make the whole static IP configuration fail. The real result is reported by the readback in tk_net_query.
///
/// Also note: values in SCDynamicStore are **held by the session that set them**, and once the session is released the values are gone.
/// So a process-wide long-lived store is used here (see the explanation of SharedDynamicStore),
/// and it must never be changed to a locally created one.
void TryPublishDns(SCDynamicStoreRef store, std::string_view service_id,
                   const std::vector<std::string>& servers) {
  if (servers.empty()) {
    return;
  }

  std::vector<ScopedCFRef<CFStringRef>> owned;
  std::vector<const void*> raw;
  owned.reserve(servers.size());
  raw.reserve(servers.size());
  for (const std::string& server : servers) {
    owned.push_back(MakeCFString(server));
    raw.push_back(owned.back().Get());
  }

  const ScopedCFRef<CFArrayRef> addresses{::CFArrayCreate(
      kCFAllocatorDefault, raw.data(), static_cast<CFIndex>(raw.size()), &kCFTypeArrayCallBacks)};
  const void* keys[] = {CFSTR("ServerAddresses")};
  const void* values[] = {addresses.Get()};
  const ScopedCFRef<CFDictionaryRef> payload{
      ::CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks)};

  const ScopedCFRef<CFStringRef> key =
      MakeCFString(std::format("State:/Network/Service/{}/DNS", service_id));
  if (::SCDynamicStoreSetValue(store, key.Get(), payload.Get()) == 0) {
    TETHERKITNEXT_WARN_TR(Msg::kCapiDnsPublishFailed, service_id);
  }
}

/// Waits for the **newly created** service to publish a **stable** lease, and returns its gateway.
///
/// The judgment uses `InterfaceName` + `Addresses` + `Router` in `State:/Network/Service/<id>/IPv4`,
/// rather than looking only at the interface address: when an old service has just been removed, the interface still carries the previous address.
///
/// * Why we also wait for "stable" *
///   Android's USB tethering **first** issues a 464XLAT transitional lease
///   (192.0.0.2 / gateway 192.0.0.1), and only after a few seconds switches to the real 172.19.x.x range. Measured, installing a
///   scoped route from the first one leaves a route pointing to an invalidated gateway after the device changes leases --
///   and that is exactly the route NetworkExtension uses when it looks up paths per interface. So the gateway must read the same value
///   kDhcpStableSamples times in a row to count as stable.
///
/// WARNING: Do not query `ConfigMethod` or the `State` in the DHCP dictionary -- **the dynamic store entries of persistent services
/// do not have these two keys** (measured: a registered service has only Addresses / Router / InterfaceName /
/// AdditionalRoutes, and the DHCP dictionary has only Lease* and Option_*; those two keys appear only on the temporary service that
/// `ipconfig set` establishes).
///
/// `service_id` is nullopt on the transient-service fallback (`ipconfig set
/// DHCP`), whose ID is only known once IPConfiguration publishes it; it is then
/// looked up by InterfaceName on every poll.
[[nodiscard]] std::optional<std::string> WaitForStableLease(
    std::string_view interface_name, const std::optional<std::string>& known_service_id) {
  SCDynamicStoreRef store = SharedDynamicStore();
  if (store == nullptr) {
    return std::nullopt;
  }

  // Read once "the gateway this service currently publishes"; if the address and interface name do not match, treat it as not ready yet.
  const auto published_router = [&]() -> std::optional<std::string> {
    const std::optional<std::string> address = QueryAddress(interface_name, SIOCGIFADDR);
    if (!address.has_value()) {
      return std::nullopt;
    }
    const std::optional<std::string> service_id =
        known_service_id.has_value() ? known_service_id : FindServiceId(store, interface_name);
    if (!service_id.has_value()) {
      return std::nullopt;
    }
    const ScopedCFRef<CFDictionaryRef> ipv4 = CopyServiceEntry(store, *service_id, "IPv4");
    if (StringField(ipv4.Get(), CFSTR("InterfaceName")) != interface_name) {
      return std::nullopt;
    }
    const std::vector<std::string> addresses = StringArrayField(ipv4.Get(), CFSTR("Addresses"));
    if (std::ranges::find(addresses, *address) == addresses.end()) {
      return std::nullopt;
    }
    const std::string router = StringField(ipv4.Get(), CFSTR("Router"));
    return IsValidIpv4(router) ? std::optional{router} : std::nullopt;
  };

  std::optional<std::string> candidate;
  int stable_samples = 0;
  const auto deadline = std::chrono::steady_clock::now() + kDhcpLeaseTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const std::optional<std::string> router = published_router();
    if (router.has_value() && router == candidate) {
      if (++stable_samples >= kDhcpStableSamples) {
        return candidate;
      }
    } else {
      candidate = router;
      stable_samples = router.has_value() ? 1 : 0;
    }
    std::this_thread::sleep_for(kDhcpPollInterval);
  }
  return std::nullopt;
}

/// Installs a default route **bound to this interface**.
///
/// A scoped route (RTF_IFSCOPE) is macOS's per-interface independent default route: traffic bound to that interface uses it,
/// without affecting the system's primary service. IPConfiguration usually installs this one for DHCP services;
/// but when feth becomes the primary service, macOS may keep only the global default route. It is explicitly filled in here, ensuring that
/// callers such as NetworkExtension that do route lookups per interface can still find a path.
///
/// Delete first and then add, without using `change`: the interface may have a leftover scoped route pointing to the **old gateway**
/// (the device changed leases), and `change` has ambiguous semantics for one with the same destination but already invalid,
/// so deleting and rebuilding is the only way to guarantee exactly one at the end, pointing to the current gateway. A delete failure usually means there was none to begin with,
/// which is a normal situation, so its result is ignored.
[[nodiscard]] Status InstallScopedDefaultRoute(std::string_view interface_name,
                                               std::string_view router) {
  const std::vector<std::string> delete_arguments{
      "-n", "delete", "-inet", "-ifscope", std::string{interface_name}, "default"};
  std::ignore = RunTool(kRoutePath, delete_arguments);

  const std::vector<std::string> add_arguments{
      "-n", "add", "-inet", "-ifscope", std::string{interface_name}, "default", std::string{router}};
  return RunOrFail(kRoutePath, add_arguments, Text(Msg::kCapiWhatAddScopedRoute));
}

/// Changes the **global** default route to the specified gateway.
///
/// ⚠️ On its own this is not enough to send "all traffic" through the phone:
/// it changes the kernel route but not the DNS resolvers (those follow the
/// primary *service*), and configd reinstalls the primary service's route on
/// the next network change. In DHCP mode the real mechanism is putting the
/// managed service first in the service order (ConfigureManagedDhcpService);
/// this call only makes the switch immediate, and is the whole story only on
/// the transient-service fallback and in static mode.
[[nodiscard]] Status PromoteToGlobalDefaultRoute(std::string_view router) {
  const std::vector<std::string> arguments{"-n", "change", "-inet", "default", std::string{router}};
  if (const auto status = RunOrFail(kRoutePath, arguments, Text(Msg::kCapiWhatSwitchGlobalRoute));
      status) {
    return tetherkitnext::Ok();
  }
  // The system may currently have no global default route at all (not connected to any network), in which case change fails,
  // and add should be used. This is exactly the most typical USB tethering scenario and must be handled.
  const std::vector<std::string> add_arguments{"-n", "add", "-inet", "default",
                                               std::string{router}};
  return RunOrFail(kRoutePath, add_arguments, Text(Msg::kCapiWhatAddGlobalRoute));
}



[[nodiscard]] Status ApplyDhcp(std::string_view interface_name, bool set_default_route) {
  // First clear the old service on the same interface. `ipconfig NONE` handles the temporary service left over from before the upgrade;
  // the SCNetworkService is the service that DHCP actually uses this time.
  TETHERKITNEXT_RETURN_IF_ERROR(tetherkitnext::capi::RemoveManagedNetworkService(interface_name));
  TETHERKITNEXT_RETURN_IF_ERROR(
      RunOrFail(kIpconfigPath, {"set", std::string{interface_name}, "NONE"},
                Text(Msg::kCapiWhatClearConfig)));

  std::optional<std::string> service_id;
  if (tetherkitnext::capi::ManagedNetworkServiceAvailable()) {
    TETHERKITNEXT_ASSIGN_OR_RETURN(
        service_id,
        tetherkitnext::capi::ConfigureManagedDhcpService(interface_name, set_default_route));
  } else {
    // SPI gone on this macOS: fall back to the transient IPConfiguration
    // service. Ordinary traffic works; only NetworkExtension VPNs lose the
    // interface-scoped path (upstream XiaoMiku01/TetherKit#3).
    TETHERKITNEXT_RETURN_IF_ERROR(
        RunOrFail(kIpconfigPath, {"set", std::string{interface_name}, "DHCP"},
                  Text(Msg::kCapiWhatStartDhcp)));
  }

  // Wait for a stable lease. IPConfiguration gets the lease, configures scoped DNS, and publishes the service to
  // the dynamic store; the scoped default route must be filled in explicitly by us (when feth becomes the primary service macOS may keep only the
  // global route, and NetworkExtension's scoped query for feth would get "No network route").
  const std::optional<std::string> router = WaitForStableLease(interface_name, service_id);
  if (!router.has_value()) {
    // There is no address yet -- the other side is probably not doing tethering, and this is the real timeout.
    if (!QueryAddress(interface_name, SIOCGIFADDR).has_value()) {
      return std::unexpected(Error::Generic(Tr(Msg::kCapiDhcpTimeout, kDhcpLeaseTimeout.count())));
    }
    // There is an address but the gateway never stabilized: install no default route, to avoid leaving one that points to a stale gateway.
    if (set_default_route) {
      return std::unexpected(Error::Generic(Tr(Msg::kCapiDhcpNoRouter)));
    }
    return tetherkitnext::Ok();
  }

  TETHERKITNEXT_RETURN_IF_ERROR(InstallScopedDefaultRoute(interface_name, *router));
  if (!set_default_route) {
    return tetherkitnext::Ok();
  }
  return PromoteToGlobalDefaultRoute(*router);
}

[[nodiscard]] Status ApplyManual(std::string_view interface_name, const tk_ip_config_t& config) {
  if (!IsValidIpv4(config.address)) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kCapiInvalidAddress, config.address)));
  }
  if (!IsValidIpv4(config.netmask)) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kCapiInvalidNetmask, config.netmask)));
  }
  const std::string_view router{config.router};
  if (!router.empty() && !IsValidIpv4(router)) {
    return std::unexpected(
        Error::Generic(Tr(Msg::kCapiInvalidRouter, router)));
  }
  if (config.set_default_route && router.empty()) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiDefaultRouteNeedsRouter)));
  }

  std::vector<std::string> dns_servers;
  for (std::int32_t i = 0; i < config.dns_count && i < TK_DNS_MAX; ++i) {
    const std::string_view server{config.dns[i]};
    if (server.empty()) {
      continue;
    }
    if (!IsValidIpv4(server)) {
      return std::unexpected(
          Error::Generic(Tr(Msg::kCapiInvalidDns, server)));
    }
    dns_servers.emplace_back(server);
  }

  TETHERKITNEXT_RETURN_IF_ERROR(tetherkitnext::capi::RemoveManagedNetworkService(interface_name));
  // The address is still applied via IPConfiguration; see the file header for the reason.
  TETHERKITNEXT_RETURN_IF_ERROR(RunOrFail(
      kIpconfigPath,
      {"set", std::string{interface_name}, "MANUAL", config.address, config.netmask},
      Text(Msg::kCapiWhatApplyManual)));

  if (!router.empty()) {
    TETHERKITNEXT_RETURN_IF_ERROR(InstallScopedDefaultRoute(interface_name, router));
    if (config.set_default_route) {
      TETHERKITNEXT_RETURN_IF_ERROR(PromoteToGlobalDefaultRoute(router));
    }
  }

  if (!dns_servers.empty()) {
    SCDynamicStoreRef store = SharedDynamicStore();
    if (store != nullptr) {
      if (const std::optional<std::string> service_id = FindServiceId(store, interface_name);
          service_id.has_value()) {
        TryPublishDns(store, *service_id, dns_servers);
      } else {
        TETHERKITNEXT_WARN_TR(Msg::kCapiNoServiceForDns, interface_name);
      }
    }
  }
  return tetherkitnext::Ok();
}

[[nodiscard]] Status ApplyNone(std::string_view interface_name) {
  TETHERKITNEXT_RETURN_IF_ERROR(tetherkitnext::capi::RemoveManagedNetworkService(interface_name));
  return RunOrFail(kIpconfigPath, {"set", std::string{interface_name}, "NONE"},
                   Text(Msg::kCapiWhatClearConfig));
}

}  // namespace

void tk_ip_config_init(tk_ip_config_t* out_config) {
  if (out_config == nullptr) {
    return;
  }
  *out_config = tk_ip_config_t{};
  // Default DHCP: RNDIS devices almost always come with their own DHCP server, and this is the only option most users need.
  out_config->mode = TK_IP_MODE_DHCP;
  // Default is **not** to grab the global default route. It is only needed when a higher-priority connected service exists at the same time,
  // while the typical USB tethering scenario is precisely that no other network is available -- then this NIC is naturally the primary service.
  out_config->set_default_route = false;
}

tk_result_t tk_net_apply(const char* interface_name, const tk_ip_config_t* config,
                         tk_error_t* out_error) {
  ClearError(out_error);
  if (config == nullptr) {
    FillGenericError(out_error, Tr(Msg::kCapiApplyConfigNull));
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (const auto status = ValidateInterface(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (::geteuid() != 0) {
    FillGenericError(out_error, Tr(Msg::kCapiApplyNeedsRoot));
    return TK_ERR_PERMISSION;
  }

  Status status = tetherkitnext::Ok();
  switch (config->mode) {
    case TK_IP_MODE_DHCP:
      status = ApplyDhcp(interface_name, config->set_default_route);
      break;
    case TK_IP_MODE_MANUAL:
      status = ApplyManual(interface_name, *config);
      break;
    case TK_IP_MODE_NONE:
      status = ApplyNone(interface_name);
      break;
    default:
      FillGenericError(out_error, Tr(Msg::kCapiUnknownIpMode, config->mode));
      return TK_ERR_INVALID_ARGUMENT;
  }

  if (!status) {
    FillError(out_error, status.error());
    return TK_ERR_FAILED;
  }
  return TK_OK;
}

tk_result_t tk_net_clear(const char* interface_name, tk_error_t* out_error) {
  ClearError(out_error);
  if (const auto status = ValidateInterface(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (::geteuid() != 0) {
    FillGenericError(out_error, Tr(Msg::kCapiClearNeedsRoot));
    return TK_ERR_PERMISSION;
  }

  // First remove the DNS key we wrote ourselves (if any), then let IPConfiguration tear down the service.
  // With the order reversed the service would already be gone, and the key would become an orphan nobody claims.
  if (SCDynamicStoreRef store = SharedDynamicStore(); store != nullptr) {
    if (const std::optional<std::string> service_id = FindServiceId(store, interface_name);
        service_id.has_value()) {
      const ScopedCFRef<CFStringRef> key =
          MakeCFString(std::format("State:/Network/Service/{}/DNS", *service_id));
      ::SCDynamicStoreRemoveValue(store, key.Get());
    }
  }

  if (const auto status = ApplyNone(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_FAILED;
  }
  return TK_OK;
}

tk_result_t tk_net_query(const char* interface_name, tk_net_state_t* out_state,
                         tk_error_t* out_error) {
  ClearError(out_error);
  if (out_state == nullptr) {
    return TK_ERR_INVALID_ARGUMENT;
  }
  if (const auto status = ValidateInterface(interface_name); !status) {
    FillError(out_error, status.error());
    return TK_ERR_INVALID_ARGUMENT;
  }
  *out_state = tk_net_state_t{};

  // ---- Address and mask: the real state in the kernel ----
  if (const std::optional<std::string> address = QueryAddress(interface_name, SIOCGIFADDR);
      address.has_value()) {
    out_state->has_address = true;
    CopyText(out_state->address, *address);
  }
  if (const std::optional<std::string> netmask = QueryAddress(interface_name, SIOCGIFNETMASK);
      netmask.has_value()) {
    CopyText(out_state->netmask, *netmask);
  }

  // ---- Service-level information: gateway, DNS, configuration method ----
  SCDynamicStoreRef store = SharedDynamicStore();
  if (store == nullptr) {
    // Failing to get the dynamic store does not count as a query failure -- the address half is already in hand, so return as usual.
    return TK_OK;
  }

  const std::optional<std::string> service_id = FindServiceId(store, interface_name);
  if (service_id.has_value()) {
    const ScopedCFRef<CFDictionaryRef> ipv4 = CopyServiceEntry(store, *service_id, "IPv4");
    const std::string router = StringField(ipv4.Get(), CFSTR("Router"));
    if (!router.empty()) {
      CopyText(out_state->router, router);
      out_state->has_default_route = true;
    }
    // ConfigMethod / State are published only by temporary services; persistent services do not have these two keys, so here we infer the configuration method and service state from
    // "whether the DHCP lease dictionary exists", to keep the UI from showing blanks.
    //
    // WARNING: The criterion must be **the dictionary itself**; do not read LeaseStartTime inside it -- that value is a
    // **CFDate, not a CFString**, so StringField would always return an empty string, and every DHCP
    // service would then be misjudged as MANUAL (this wrong badge really did appear in the UI).
    const ScopedCFRef<CFDictionaryRef> dhcp = CopyServiceEntry(store, *service_id, "DHCP");

    std::string method = StringField(ipv4.Get(), CFSTR("ConfigMethod"));
    if (method.empty() && out_state->has_address) {
      method = dhcp ? "DHCP" : "MANUAL";
    }
    CopyText(out_state->method, method);

    std::string service_state = StringField(dhcp.Get(), CFSTR("State"));
    if (service_state.empty() && dhcp && out_state->has_address) {
      service_state = "BOUND";
    }
    CopyText(out_state->service_state, service_state);

    // DNS is always read back from **what is actually in effect in the system**, rather than restating the values we applied --
    // in static mode whether DNS takes effect depends on whether IPMonitor accepts it, and only a readback is accurate.
    const ScopedCFRef<CFDictionaryRef> dns = CopyServiceEntry(store, *service_id, "DNS");
    const std::vector<std::string> servers = StringArrayField(dns.Get(), CFSTR("ServerAddresses"));
    for (const std::string& server : servers) {
      if (out_state->dns_count >= TK_DNS_MAX) {
        break;
      }
      CopyText(out_state->dns[out_state->dns_count], TK_ADDRESS_CAPACITY, server);
      ++out_state->dns_count;
    }
  }

  out_state->is_primary_default_route = PrimaryInterface(store) == interface_name;
  return TK_OK;
}
