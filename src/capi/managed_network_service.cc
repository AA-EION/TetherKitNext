// Register feth as a real macOS network service while TetherKitNext owns it.
//
// `ipconfig set <if> DHCP` publishes a dynamic State:/ service, but no
// Setup:/Network/Service entry. Packet-tunnel providers can lose that transient
// interface when NetworkExtension takes over routing. A service in the current
// SCNetworkSet gives configd the durable interface metadata it needs to preserve
// the underlying path during tunnel startup.
#include "managed_network_service.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <dlfcn.h>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "capi_support.h"
#include "core_foundation_support.h"
#include "tetherkitnext/common/i18n.h"

namespace {

using tetherkitnext::Error;
using tetherkitnext::Msg;
using tetherkitnext::Status;
using tetherkitnext::Tr;
using tetherkitnext::capi::CopyToStdString;
using tetherkitnext::capi::IsValidFethName;
using tetherkitnext::capi::MakeCFString;
using tetherkitnext::capi::ScopedCFRef;

// SystemConfiguration exports this SPI on every supported macOS version. The
// public API can enumerate only IOKit-backed interfaces, while feth is a pure
// ifnet. The SPI constructs the same Ethernet SCNetworkInterface object from a
// BSD name; all subsequent service/configuration calls are public API.
// Source: apple-oss-distributions/configd, SCNetworkConfigurationPrivate.h.
//
// Resolved at runtime with dlsym rather than linked directly: a hard reference
// to an SPI means that if a future macOS drops the symbol, dyld refuses to load
// libtetherkitnext at all and the whole app/helper dies at launch. Looked up
// lazily, a missing symbol only disables the managed service and DHCP falls
// back to the transient `ipconfig set` path.
using CreateWithBsdNameFunction = SCNetworkInterfaceRef (*)(CFAllocatorRef allocator,
                                                            CFStringRef bsd_name,
                                                            std::uint32_t flags);

[[nodiscard]] CreateWithBsdNameFunction ResolveCreateWithBsdName() noexcept {
  static const CreateWithBsdNameFunction function = [] {
    void* const symbol = ::dlsym(RTLD_DEFAULT, "_SCNetworkInterfaceCreateWithBSDName");
    return reinterpret_cast<CreateWithBsdNameFunction>(symbol);  // NOLINT
  }();
  return function;
}

constexpr std::uint32_t kIncludeAllVirtualInterfaces = 0xFFFF'FFFFU;
// Services are named "TetherKitNext (fethN)". Matching on the shorter
// "TetherKit" prefix also catches the "TetherKit (fethN)" services left behind
// by builds from before the rename, so orphan cleanup removes those too. A
// service only counts as ours if it is also bound to a feth interface.
constexpr std::string_view kManagedServicePrefix = "TetherKit";

[[nodiscard]] std::unexpected<Error> SystemConfigurationFailure(
    std::string_view operation) {
  const int code = ::SCError();
  const char* const reason = ::SCErrorString(code);
  return std::unexpected(Error::Generic(Tr(
      Msg::kCapiSystemConfigurationFailed, operation, code,
      reason != nullptr ? std::string_view{reason} : std::string_view{"unknown error"})));
}

[[nodiscard]] bool IsManagedService(SCNetworkServiceRef service,
                                    std::optional<std::string_view> interface_name) {
  const CFStringRef name = ::SCNetworkServiceGetName(service);
  const ScopedCFRef<CFStringRef> prefix = MakeCFString(kManagedServicePrefix);
  if (name == nullptr || !::CFStringHasPrefix(name, prefix.Get())) {
    return false;
  }

  const SCNetworkInterfaceRef interface = ::SCNetworkServiceGetInterface(service);
  if (interface == nullptr) {
    return false;
  }
  const std::string bsd_name = CopyToStdString(::SCNetworkInterfaceGetBSDName(interface));
  if (!IsValidFethName(bsd_name)) {
    return false;
  }
  return !interface_name.has_value() || bsd_name == *interface_name;
}

[[nodiscard]] Status RemoveMatchingServices(
    SCPreferencesRef preferences, std::optional<std::string_view> interface_name,
    bool& changed) {
  const ScopedCFRef<CFArrayRef> services{::SCNetworkServiceCopyAll(preferences)};
  if (!services) {
    return SystemConfigurationFailure("SCNetworkServiceCopyAll");
  }

  const CFIndex count = ::CFArrayGetCount(services.Get());
  for (CFIndex index = 0; index < count; ++index) {
    const auto service = static_cast<SCNetworkServiceRef>(
        ::CFArrayGetValueAtIndex(services.Get(), index));
    if (!IsManagedService(service, interface_name)) {
      continue;
    }
    if (::SCNetworkServiceRemove(service) == 0) {
      return SystemConfigurationFailure("SCNetworkServiceRemove");
    }
    changed = true;
  }
  return tetherkitnext::Ok();
}

[[nodiscard]] Status CommitAndApply(SCPreferencesRef preferences) {
  if (::SCPreferencesCommitChanges(preferences) == 0) {
    return SystemConfigurationFailure("SCPreferencesCommitChanges");
  }
  if (::SCPreferencesApplyChanges(preferences) == 0) {
    return SystemConfigurationFailure("SCPreferencesApplyChanges");
  }
  return tetherkitnext::Ok();
}

[[nodiscard]] ScopedCFRef<SCPreferencesRef> CreatePreferences() {
  return ScopedCFRef<SCPreferencesRef>{
      ::SCPreferencesCreate(kCFAllocatorDefault, CFSTR("TetherKitNext"), nullptr)};
}

/// Holds the SCPreferences write lock for a scope.
///
/// Apple requires writers of the network preferences to take this lock so a
/// concurrent writer (System Settings, networksetup, configd itself) cannot
/// interleave with our read-modify-commit. `wait = true` blocks until the
/// other writer finishes instead of failing. Unlock is implicit on commit
/// failure paths via the destructor.
class PreferencesLock {
 public:
  explicit PreferencesLock(SCPreferencesRef preferences) noexcept
      : preferences_(preferences), locked_(::SCPreferencesLock(preferences, true) != 0) {}
  PreferencesLock(const PreferencesLock&) = delete;
  PreferencesLock& operator=(const PreferencesLock&) = delete;
  PreferencesLock(PreferencesLock&&) = delete;
  PreferencesLock& operator=(PreferencesLock&&) = delete;
  ~PreferencesLock() {
    if (locked_) {
      ::SCPreferencesUnlock(preferences_);
    }
  }
  [[nodiscard]] bool Locked() const noexcept { return locked_; }

 private:
  SCPreferencesRef preferences_;
  bool locked_;
};

[[nodiscard]] Status RemoveManagedServices(
    std::optional<std::string_view> interface_name) {
  const ScopedCFRef<SCPreferencesRef> preferences = CreatePreferences();
  if (!preferences) {
    return SystemConfigurationFailure("SCPreferencesCreate");
  }

  const PreferencesLock lock{preferences.Get()};
  if (!lock.Locked()) {
    return SystemConfigurationFailure("SCPreferencesLock");
  }

  bool changed = false;
  TETHERKITNEXT_RETURN_IF_ERROR(
      RemoveMatchingServices(preferences.Get(), interface_name, changed));
  return changed ? CommitAndApply(preferences.Get()) : tetherkitnext::Ok();
}

}  // namespace

namespace tetherkitnext::capi {

bool ManagedNetworkServiceAvailable() noexcept {
  return ResolveCreateWithBsdName() != nullptr;
}

Result<std::string> ConfigureManagedDhcpService(std::string_view interface_name,
                                                bool make_primary) {
  const CreateWithBsdNameFunction create_with_bsd_name = ResolveCreateWithBsdName();
  if (create_with_bsd_name == nullptr) {
    return std::unexpected(Error::Generic(Tr(Msg::kCapiSystemConfigurationFailed,
                                             "_SCNetworkInterfaceCreateWithBSDName", 0,
                                             "symbol not available on this macOS")));
  }
  const ScopedCFRef<SCPreferencesRef> preferences = CreatePreferences();
  if (!preferences) {
    return SystemConfigurationFailure("SCPreferencesCreate");
  }
  const PreferencesLock lock{preferences.Get()};
  if (!lock.Locked()) {
    return SystemConfigurationFailure("SCPreferencesLock");
  }

  bool removed_existing = false;
  TETHERKITNEXT_RETURN_IF_ERROR(
      RemoveMatchingServices(preferences.Get(), interface_name, removed_existing));

  const ScopedCFRef<CFStringRef> bsd_name = MakeCFString(interface_name);
  const ScopedCFRef<SCNetworkInterfaceRef> interface{
      create_with_bsd_name(kCFAllocatorDefault, bsd_name.Get(), kIncludeAllVirtualInterfaces)};
  if (!interface) {
    return SystemConfigurationFailure("_SCNetworkInterfaceCreateWithBSDName");
  }

  const ScopedCFRef<SCNetworkServiceRef> service{
      ::SCNetworkServiceCreate(preferences.Get(), interface.Get())};
  if (!service) {
    return SystemConfigurationFailure("SCNetworkServiceCreate");
  }

  const ScopedCFRef<CFStringRef> service_name =
      MakeCFString(std::format("TetherKitNext ({})", interface_name));
  if (::SCNetworkServiceSetName(service.Get(), service_name.Get()) == 0) {
    return SystemConfigurationFailure("SCNetworkServiceSetName");
  }
  if (::SCNetworkServiceEstablishDefaultConfiguration(service.Get()) == 0) {
    return SystemConfigurationFailure("SCNetworkServiceEstablishDefaultConfiguration");
  }
  if (::SCNetworkServiceSetEnabled(service.Get(), true) == 0) {
    return SystemConfigurationFailure("SCNetworkServiceSetEnabled");
  }

  const ScopedCFRef<SCNetworkProtocolRef> ipv4{
      ::SCNetworkServiceCopyProtocol(service.Get(), kSCNetworkProtocolTypeIPv4)};
  if (!ipv4) {
    return SystemConfigurationFailure("SCNetworkServiceCopyProtocol(IPv4)");
  }
  const void* keys[] = {kSCPropNetIPv4ConfigMethod};
  const void* values[] = {kSCValNetIPv4ConfigMethodDHCP};
  const ScopedCFRef<CFDictionaryRef> configuration{::CFDictionaryCreate(
      kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks)};
  if (::SCNetworkProtocolSetConfiguration(ipv4.Get(), configuration.Get()) == 0) {
    return SystemConfigurationFailure("SCNetworkProtocolSetConfiguration(IPv4)");
  }

  const ScopedCFRef<SCNetworkSetRef> network_set{
      ::SCNetworkSetCopyCurrent(preferences.Get())};
  if (!network_set) {
    return SystemConfigurationFailure("SCNetworkSetCopyCurrent");
  }
  if (::SCNetworkSetAddService(network_set.Get(), service.Get()) == 0) {
    return SystemConfigurationFailure("SCNetworkSetAddService");
  }

  // SCNetworkSetAddService appends the service to the *end* of the set's
  // service order, so without this step Ethernet/Wi-Fi always outrank the
  // phone. The order is what IPMonitor uses to pick the primary service, and
  // the primary service owns both the global default route *and* the DNS
  // resolver configuration. Putting ours first is therefore the only way to
  // send all traffic (including name lookups) through the phone when another
  // network — e.g. a LAN without internet access — is also connected.
  // It is the same setting as System Settings › Network › Set Service Order.
  // Services are recreated on every apply, so turning the option off simply
  // leaves the new service at the end again.
  if (make_primary) {
    const CFStringRef our_id = ::SCNetworkServiceGetServiceID(service.Get());
    const CFArrayRef current_order = ::SCNetworkSetGetServiceOrder(network_set.Get());
    const ScopedCFRef<CFMutableArrayRef> new_order{
        ::CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks)};
    ::CFArrayAppendValue(new_order.Get(), our_id);
    if (current_order != nullptr) {
      const CFIndex count = ::CFArrayGetCount(current_order);
      for (CFIndex i = 0; i < count; ++i) {
        const void* const id = ::CFArrayGetValueAtIndex(current_order, i);
        if (::CFEqual(id, our_id) == 0) {
          ::CFArrayAppendValue(new_order.Get(), id);
        }
      }
    }
    if (::SCNetworkSetSetServiceOrder(network_set.Get(), new_order.Get()) == 0) {
      return SystemConfigurationFailure("SCNetworkSetSetServiceOrder");
    }
  }

  const std::string service_id = CopyToStdString(::SCNetworkServiceGetServiceID(service.Get()));
  TETHERKITNEXT_RETURN_IF_ERROR(CommitAndApply(preferences.Get()));
  return service_id;
}

Status RemoveManagedNetworkService(std::string_view interface_name) {
  return RemoveManagedServices(interface_name);
}

Status RemoveAllManagedNetworkServices() {
  return RemoveManagedServices(std::nullopt);
}

}  // namespace tetherkitnext::capi
