// Network configuration helpers shared inside the C ABI implementation.
#pragma once

#include <string_view>

namespace tetherkitnext::capi {

/// Makes the kernel's IPv4 default route match what configd says it should be.
///
/// "Route all traffic" in manual mode (and on the transient `ipconfig set DHCP`
/// fallback) changes the kernel's default route directly, so configd never sees
/// our interface as the primary service. When the configuration is cleared or
/// the interface is destroyed, the kernel drops that route and configd, whose
/// primary service never changed, has no reason to reinstall the Wi-Fi/Ethernet
/// one: the Mac would be left without a default route until the next network
/// change.
///
/// Reads `State:/Network/Global/IPv4` (configd's current primary interface and
/// router) and, only if the kernel route is missing or still goes through
/// `interface_name`, points it back at that router. Never touches a route
/// owned by something else (e.g. a VPN), and does nothing while configd itself
/// considers `interface_name` primary (it reroutes on its own when our service
/// goes away). Idempotent, best effort, never throws.
void ReconcileSystemDefaultRoute(std::string_view interface_name) noexcept;

}  // namespace tetherkitnext::capi
