# Local-link devices (not phones)

Many devices use the same USB networking (RNDIS) as Android tethering but are
a **private link to the device**, not an internet connection:
- drones and their remote controllers, whose desktop companion apps update
  firmware, calibrate and download logs over this link;
- FPV goggles;
- single-board computers in USB gadget mode (Raspberry Pi Zero, BeagleBone);
- cameras and other appliances.

On macOS they used to need HoRNDIS, a kernel extension that is unmaintained
and doesn't load on current macOS without disabling SIP. Some vendors' install
guides tell users to do exactly that. TetherKitNext's user-space driver already
speaks RNDIS to any device that matches the standard interface signatures
(no vendor table), so the work here is about **behaving correctly once
connected**.

## What goes wrong without this

| Device behavior | Old result |
|---|---|
| No DHCP server (fixed address) | "DHCP got no lease within 10 s" alert: looks broken. |
| DHCP without a router | Connected but "no internet". With *Route all traffic* on, our service stayed **first in the service order**, so the device could outrank the Mac's real internet. |
| Fixed address | The user had to guess the device's subnet. |

## Design

1. **Live link assessment** (`TetherKitNextUIModel/LinkAssessment.swift`,
   unit-tested). From current facts only (run state, time since connect,
   DHCP lease, router, the device's own address) it yields: *waiting for an
   address*, *internet* (lease with a router), *local link* (address, no
   router; includes macOS's 169.254 fallback) or *no address* (after a grace
   period). Phones with internet see exactly the UI they saw before.
2. **Passive peer discovery** (`core/peer_address.h`). The bridge reads the
   device's own IPv4 address from frames only it can have sent: ARP sender
   addresses, plus IPv4 sent to the limited broadcast or 224.0.0.0/24 (mDNS,
   DHCP replies). Ordinary unicast is ignored, because phones forward internet
   traffic whose source is a remote server. This costs a few byte loads per
   frame and one atomic store when the address changes, and sends nothing.
   It is exposed as `tk_session_status_t.peer_ipv4` and `SessionStatus.peerAddress`
   (optional, so older daemons' JSON still decodes).
3. **Route safety.** If *Route all traffic* was requested but no router
   appears, `ApplyDhcp` moves our service back to the end of the service order
   (`DemoteManagedNetworkService`). A device that can't reach the internet
   never outranks Wi-Fi or Ethernet. The UI says the option wasn't applied.
4. **Quiet auto-configuration.** DHCP timeouts after Connect are no longer
   alerts; the card explains the state instead. An explicit *Apply* on the
   Network page still reports errors.
5. **Session-only address.** For devices without DHCP, the card suggests an
   address in the device's /24 (`.10`, `.20`, `.100` or `.200`, never the
   device's own) or takes one typed in. It is applied as a manual
   configuration for this connection only. It is never written to the saved
   settings and is cleared when the session ends. macOS's IPConfiguration
   checks for address conflicts with ARP when it applies the address.

## Why the device kind is never saved

A remembered label misfires: a phone that is slow to start its DHCP server,
or whose upstream (Wi-Fi bridging, mobile data) is slow or down, would be
saved as "local device", and the next connection would be handled wrongly.
Alternatives considered:

| Option | Verdict |
|---|---|
| Remember kind per device (VID:PID + serial) | **Rejected.** A slow phone gets stuck with the wrong label, and Android randomizes the RNDIS MAC every session. |
| Vendor table (e.g. "this vendor ID is a drone") | **Rejected for behavior.** Same vendor IDs cover phones and accessories, and it ages badly. It could at most be a UI hint, and isn't needed. |
| Probe the internet (HTTP / DNS check through the interface) | **Rejected.** It sends traffic the user didn't ask for, and it fails on a phone whose upstream is momentarily down, which is exactly the slow-phone misfire. |
| **Evidence-based, recomputed every poll** | **Chosen.** Only facts the link itself reports count (lease, router, the device's address). A late lease flips the result to *internet* with no user action. Nothing is stored. |
| Grace period before saying "no address" | **Chosen** (20 s): covers the daemon's 10 s DHCP wait, Android's transitional 464XLAT lease and slow devices. |
| Manual per-session override ("treat as phone / local") | Not needed: the only behavior that depends on the kind is *Route all traffic*, and that follows the factual presence of a router. "Try automatic again" re-applies the saved settings at any time. |
| Session-only address instead of editing saved settings | **Chosen.** Configuring a drone must never leave a fixed address behind for the next phone. |

## Testing

- C++: `core.peer_address` (frame rules, observer) and `capi.basics`
  (`peer_ipv4` empty when idle). These run in CI on every macOS runner.
- Swift: `LinkAssessmentTests` (grace period, late lease, 169.254 fallback,
  suggestions).
- **Not yet tested on hardware**: a drone or controller with its vendor
  software, and a Raspberry Pi Zero in gadget mode with a static address.
  A gadget-mode Pi is the easiest stand-in:
  `modprobe g_ether` (RNDIS enabled), `ip addr add 192.168.7.2/24 dev usb0`,
  and no DHCP server.
