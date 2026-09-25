// TetherKit-owned SystemConfiguration network-service lifecycle.
#pragma once

#include <string>
#include <string_view>

#include "tetherkit/common/error.h"

namespace tetherkit::capi {

/// Whether the SystemConfiguration SPI needed to build a service for a feth
/// interface exists on this macOS. When false, callers fall back to the
/// transient `ipconfig set <if> DHCP` service.
[[nodiscard]] bool ManagedNetworkServiceAvailable() noexcept;

/// Replace any prior TetherKit service for the interface with a DHCP service
/// registered in the current macOS network set. Returns the new service ID.
///
/// `make_primary` moves the service to the front of the set's service order,
/// making it the primary service: macOS then routes all traffic *and* DNS
/// through it, ahead of Ethernet/Wi-Fi, and keeps doing so across network
/// changes.
[[nodiscard]] Result<std::string> ConfigureManagedDhcpService(
    std::string_view interface_name, bool make_primary);

/// Remove TetherKit-owned services for one feth interface. Idempotent.
[[nodiscard]] Status RemoveManagedNetworkService(std::string_view interface_name);

/// Remove every stale TetherKit-owned feth service. Used during orphan cleanup.
[[nodiscard]] Status RemoveAllManagedNetworkServices();

}  // namespace tetherkit::capi
