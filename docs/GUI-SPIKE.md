# GUI and Privilege Escalation Feasibility Spike

This document records the findings of the initial exploratory spike: **can we build a native Swift GUI on top of TetherKitNext and acquire root privileges for network configuration cleanly on macOS?**

The answer is yes, after ruling out several approaches that initially appeared standard. **Documenting the rejected paths and why they failed is even more valuable than the final design itself**, so future maintainers do not repeat the same dead ends.

> Note: Section 3.3 and Section 4 reflect the initial upstream spike when distribution targeted unsigned source builds via Homebrew. For how TetherKitNext 1.0+ evolved to signed universal DMGs and `SMAppService.daemon`, see Section 0 of [GUI-ARCHITECTURE.md](GUI-ARCHITECTURE.md).

---

## 1. Summary of Findings

| Capability | Verdict | Evidence |
|---|---|---|
| Existing C API (at spike time) | **Did not exist** | Only C++23 headers + CLI existed; led to creating `include/tetherkitnext/capi/tetherkitnext_c.h` |
| Calling C++23 directly from Swift | **Unviable** | Swift C++ interop cannot import `std::expected`, `std::span`, or pure virtual base classes |
| USB device enumeration | **No root needed** | Verified via `libusb` as normal user (`uid 501`) |
| Creating `feth` interfaces | **Requires root** | Verified (`SIOCIFCREATE2` returns `EPERM` without root) |
| Assigning IPv4 via `SIOCAIFADDR` | **Works** | Kernel retains address without `configd` interference |
| Subnet / specific routes | **Works** | Automatically generated when IPv4 address is assigned |
| System DHCP client (`IPConfiguration`) | **Works** | Acquires lease (`State: BOUND`) on `feth0` |
| Scoped DNS & default route | **Automatically configured** | Configured by `IPConfiguration` / `SCNetworkService` |
| Overriding global default route | **Requires explicit step** | Needed only when a higher-priority interface is already online |

---

## 2. Core Fact: `feth` Has No IOKit Node

This single fact is the **common root cause** behind most macOS networking quirks with `feth`.

`if_fake` (`feth`) interfaces are created via `SIOCIFCREATE` → `ifnet_attach` and **do not register an IOKit `IONetworkInterface` provider**.

Empirical comparison on macOS:

```
IOKit IONetworkInterface:  en0–en7, anpi0/1/3, vmenet0/1   ← no feth
NetworkInterfaces.plist:   0 occurrences of feth
ipconfig getiflist:        en4 en5 en6 bridge0 en0 en7     ← no feth (until a service is attached)
SCDynamicStore:            State:/Network/Interface/feth0/Link ← present!
```

`configd`'s **dynamic store** sees `feth` link state (fed by routing sockets / kernel events), whereas `SCNetworkInterfaceCopyAll()` does **not** enumerate `feth` because it walks the IOKit registry.

---

## 3. Investigated Approaches

### 3.1 Persistent `SCPreferences` + `SCNetworkService` ✅ (Initially Thought Impossible, Later Proven Working)

Initial assumption: because `SCNetworkInterfaceCopyAll()` only enumerates IOKit nodes and omits `feth`, we initially assumed `SCNetworkService` could not be created for `feth`.

**That initial assumption was disproved.** Not being enumerated by `SCNetworkInterfaceCopyAll()` does not prevent constructing an `SCNetworkInterfaceRef`. `SystemConfiguration` provides a long-standing SPI (present in open-source `configd`'s `SCNetworkInterface.c` since macOS 10.5+) that constructs an interface object by BSD name using `SIOCGIFFLAGS` without touching IOKit:

```c
SCNetworkInterfaceRef
_SCNetworkInterfaceCreateWithBSDName(CFAllocatorRef allocator,
                                     CFStringRef    bsdName,
                                     UInt32         flags);
```

Once the `SCNetworkInterfaceRef` is constructed, public APIs (`SCNetworkServiceCreate`, `SCNetworkSetAddService`, `SCPreferencesCommitChanges`) register `feth` as a first-class service visible in **System Settings › Network** and `networksetup -listallnetworkservices`.

**Why this matters**: `ipconfig set feth0 DHCP` creates a transient `State:`-only service without a `Setup:` entry. While normal socket traffic works, **NetworkExtension VPN packet-tunnel providers (such as FortiClient) refuse to use an underlying interface unless it has a registered `Setup:` service**, failing with `"No network route to host"`. Registering a persistent `SCNetworkService` fixes VPN compatibility completely (`src/capi/managed_network_service.cc`).

### 3.2 Directly Injecting Service Keys into `SCDynamicStore` ❌

Idea: bypass `SCPreferences` and write `State:/Network/Service/<uuid>/IPv4` and `/DNS` directly into `SCDynamicStore`.

Result: `IPMonitor` ignores manually fabricated `State:` service entries unless they correspond to a service instantiated from `Setup:` or registered by `IPConfiguration` itself (`scutil --dns` never adopts the injected resolvers).

### 3.3 `SMAppService` vs. `AuthorizationRef`

- For **unsigned / source builds** (such as Homebrew formulas), `SMAppService` cannot be used because `SMPrivilegedExecutables` / `SMAuthorizedClients` require matching code-signing Designated Requirements (`cdhash` changes on every local build).
- For **signed Developer ID DMG distributions** (TetherKitNext 1.0+), `SMAppService.daemon` **is** used, combined with bidirectional Team ID code-signing checks (`CodeSigning.swift`) and `AuthorizationRef` / `LocalAuthentication` verification.

---

## 4. Privilege and Authorization Architecture

```
TetherKitNext.app (uid 501, unprivileged)
  │ ① Calls C ABI directly for non-root tasks: environment preflight, USB device list
  │ ② Confirms user authorization (Touch ID via LAContext on team-signed builds,
  │    or AuthorizationCopyRights → 32-byte external form token)
  │
  ├── XPC (com.tetherkitnext.helperd, .privileged) ──► tetherkitnext-helper (uid 0)
  │                                                    │ ③ Verifies peer Team ID signature
  │                                                    │ ④ Verifies admin group / AuthorizationRef
  │                                                    │    (WITHOUT interactionAllowed)
  │                                                    │ ⑤ Executes privileged session/network op
  └────────────────────── Result ◄─────────────────────┘
```

### Why Authorization Verification Was Chosen alongside Code Signing

| Approach | Local / Ad-Hoc Source Builds | Signed Developer ID Builds |
|---|---|---|
| `SMJobBless` Designated Requirement alone | ❌ Fails (`cdhash` changes on every build) | ✅ Works |
| `AuthorizationCopyRights` external token + Team ID check | ✅ Works (verifies `admin` credential in helper) | ✅ Works (Team ID pin + Touch ID / `AuthorizationRef`) |

### Three Critical Authorization Rules

1. **Never pass `interactionAllowed` when verifying in the daemon.** The daemon has no UI session; allowing interaction would let any local caller trigger system password dialogs.
2. **Never pass `destroyRights` to `AuthorizationFree` in the daemon.** The token belongs to the App and is cached for 5 minutes; destroying rights in the daemon would invalidate the App's cache.
3. **Do not require authorization for read-only status/probe methods.** This lets the UI distinguish between "daemon not enabled" and "user cancelled authorization".

### Historical Addendum: In-App Helper Installation via AEWP (Before 1.0 `SMAppService` Migration)

Prior to the 1.0 `SMAppService` migration, the app installed `/Library/PrivilegedHelperTools/com.tetherkitnext.helper` using `AuthorizationExecuteWithPrivileges` resolved via `dlsym`:

- **Why a binary trampoline (`--install`) was required instead of a shell script**: `/usr/libexec/security_authtrampoline` launches the child with `euid=0` and `ruid=caller`. Standard shells (`bash`, `zsh`, `dash`) drop `euid` back to `ruid` when `euid != ruid` unless `-p` is passed. Invoking the helper binary with `--install` called `setgid(0)` + `setuid(0)` first before `exec`-ing `install-helper.sh`.
- **No exit code or PID from AEWP**: AEWP returns only a stdout pipe (`stderr` was merged via `dup2`), so completion was verified by establishing a live XPC connection and checking `protocolRevision` and `buildId`.
- **Resolved via `dlsym`**: avoided hard symbol binding to a deprecated symbol.

---

## 5. Key Clarification: Authorization ≠ Root

Calling `AuthorizationCopyRights` in the App **does not change the process's `uid` or `euid`**. It returns an `AuthorizationRef` credential proving the user consented to a right. Calling `tk_feth_create()` in the App immediately after `AuthorizationCopyRights` still fails with `EPERM`; the privileged work must be performed by the root daemon (`tetherkitnext-helper`) after verifying the credential.

---

## 6. Network Configuration Capabilities

### 6.1 Kernel-Level Configuration (`SIOCAIFADDR`)

- Assigning IPv4 addresses and netmasks via `SIOCAIFADDR` works and persists without `configd` removing them.
- Subnet routes and interface-scoped default routes (`RTF_IFSCOPE`) work as expected.

### 6.2 DHCP via System `IPConfiguration` Client

macOS's built-in `IPConfiguration` agent supports `feth` interfaces directly:

```bash
sudo ipconfig set feth9 DHCP     # exit=0
ipconfig getiflist               # → ... feth9    ← appears immediately
ipconfig getsummary feth9        # → ConfigMethod: DHCP, ServiceID: DHCP-feth9
```

⚠️ **Diagnostic pitfall**: `ipconfig getiflist` only lists interfaces that **currently have an active IPConfiguration service attached**, not all configurable interfaces (even a physical Thunderbolt Ethernet card `en1` is absent from `getiflist` until configured).

### 6.3 What One Command Provides

When `feth0` is paired with `feth1` and connected to a phone's DHCP server:

```bash
sudo ipconfig set feth0 DHCP
```

`IPConfiguration` automatically completes all four steps:

| Outcome | Verification |
|---|---|
| Acquires IPv4 lease | `ipconfig getifaddr feth0` → `192.168.35.128` (`State: BOUND`) |
| Configures scoped DNS | `scutil --dns` shows `nameserver 192.168.35.7 / if_index (feth0) / Scoped` |
| Installs scoped default route | `netstat -rn` shows `default 192.168.35.7 UGScIg feth0` |
| Publishes service to `SCDynamicStore` | `State:/Network/Service/.../{DHCP,DNS,IPv4}` (`IsPublished: TRUE`) |

### 6.4 Taking Over the Global Default Route

If the Mac already has another active primary network interface (such as Wi-Fi), `feth0` receives a **scoped** default route, while unbound traffic continues using the primary interface. When the user enables "Route all traffic through this interface", the helper updates the default route (and promotes the managed service in `ServiceOrder`):

```bash
sudo route -n change default $(ipconfig getoption feth0 router)
```

### 6.5 Impact on Implementation

Because `IPConfiguration` and `SCNetworkService` handle DHCP leases, DNS publishing, and route installation natively, TetherKitNext does not need a custom user-space DHCP state machine. Teardown simply clears the `SCNetworkService` or runs `sudo ipconfig set feth0 NONE`.

---

## 7. Packaging Lessons Learned

### Hardened Runtime and `libusb`

Under Hardened Runtime, macOS refuses to load dynamic libraries signed with a different Team ID (or ad-hoc Homebrew binaries):

```
Library not loaded: /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib
Reason: ... have different Team IDs
```

Therefore, `TetherKitNext.app` bundles its own universal `libusb-1.0.0.dylib` inside `Contents/Frameworks/` and signs all Mach-O binaries with the same identity.

### Re-Signing After `install_name_tool`

On Apple Silicon, modifying load commands or rpaths with `install_name_tool` invalidates the binary's code signature, causing the kernel to immediately `SIGKILL` (`Killed: 9`) the process on launch. Always re-sign inside-out after running `install_name_tool`.

### Preserve Symlinks When Copying Dylibs (`cp -a`, Not `install`)

`libtetherkitnext.dylib` and `libtetherkitnext.0.dylib` are symlinks to the versioned library. Using `install` dereferences symlinks into separate duplicate files, causing `install_name_tool` to patch only one copy while `@rpath` loads the unpatched copy. Always use `cp -a`.

### Inspect Dependencies on the Right Binary

`libusb` is a link dependency of `libtetherkitnext.dylib`, not of the helper executable directly. Running `otool -L` against the helper executable returns no `libusb` entry, silently skipping dependency fixups if checked on the wrong target.

### Always Brace Shell Variables (`${VAR}`) Before Non-ASCII Punctuation

```bash
echo "cleaned ($IFACE)"   # Unbraced $VAR adjacent to multibyte punctuation fails under UTF-8 locale
```

Under a UTF-8 locale, `bash` treats the leading byte (`0xEF`) of full-width punctuation as a valid identifier character (`isalnum()` returns true), absorbing the punctuation into `$VAR` and failing with `unbound variable`. Always write `${VAR}`.

### Set Explicit `CMAKE_OSX_DEPLOYMENT_TARGET`

Without an explicit `CMAKE_OSX_DEPLOYMENT_TARGET`, CMake defaults to the host macOS version, and Homebrew's `libusb` is also built for the host OS version. Release builds must build `libusb` and `libtetherkitnext` with an explicit deployment target (`13.3` for CLI/core, `14.0` for GUI).

---

## 8. Homebrew Distribution Considerations

### Two Empirically Verified Constraints

- **`brew services` DSL has no `MachServices` support** (confirmed in `Library/Homebrew/service.rb`), so it cannot register an XPC LaunchDaemon.
- **`brew install` never runs as root**, so a formula cannot write to `/Library/LaunchDaemons`.

### Location of `.app` in Homebrew Cellar

A Homebrew formula can install a `.app` inside the Cellar, but it will not automatically appear in `/Applications`, Launchpad, or Spotlight without user action or a Cask / DMG drag-to-`/Applications` workflow.

### Signing Requirements by Distribution Channel

| Channel | Certificate Required? |
|---|---|
| Formula (built from source locally) | **No** — locally compiled binaries have no quarantine attribute, so Gatekeeper does not block execution |
| Prebuilt DMG / Cask (downloaded `.app`) | **Requires Developer ID + Apple Notarization** — downloaded archives carry `com.apple.quarantine` (adopted in TetherKitNext 1.0+) |

---

## 9. C API Gaps Identified During Spike (Subsequently Implemented in Commits #12–#16)

To support a non-blocking GUI and helper daemon, the following capabilities were added to `libtetherkitnext`:

- **Read-only status accessors**: bridge statistics, RNDIS state, link up/down status, negotiated parameters, driver-side interface name, and fatal error messages.
- **Self-hosted control thread (`Runtime::Start`)**: non-blocking startup instead of blocking `RunUntilStopped()`.
- **Pollable event queue and log ring (`tk_poll_event`, `tk_drain_logs`)**: avoids cross-thread Swift callbacks and re-entrancy deadlocks on `Stop()`.

| Identified Gap | Resolution in `tk_capi` |
|---|---|
| Log sink | Added lock-free ring buffer (`tk_drain_logs`) so the GUI can display live logs without reading `stderr` |
| USB string descriptors | Added vendor, product, and serial number fields (`tk_list_devices`) with `ReconcileDeviceStrings` caching |
| Hotplug | `Context::SupportsHotplug()` verified; GUI polls `tk_list_devices` every 2 seconds |
| Orphan `feth` cleanup | Added `/var/run/tetherkitnext-interfaces.state` and `tk_cleanup_orphan_interfaces` in case of `SIGKILL` |
| Helper signal handling | Added `DispatchSource.makeSignalSource` for `SIGTERM` / `SIGINT` in `tetherkitnext-helper` |

---

## 10. Useful Verification Commands

```bash
# Check whether feth appears in IOKit (expected: absent)
ioreg -c IONetworkInterface -r -d1 | grep '"BSD Name"' | sort -u

# Verify IPConfiguration accepts feth (expected: exit=0 and appears in getiflist)
sudo ifconfig feth9 create
sudo ipconfig set feth9 DHCP && ipconfig getiflist
sudo ipconfig set feth9 NONE && sudo ifconfig feth9 destroy

# ⚠️ Note: `ipconfig getiflist` lists interfaces that currently have an active
#    IPConfiguration service, NOT all interfaces capable of being configured.
#    Interfaces that have never run a service (even physical NICs) do not appear.

# End-to-end verification on a live RNDIS session
sudo ipconfig set feth0 DHCP
ipconfig getifaddr feth0                     # Lease IP
scutil --dns | grep -A3 feth0                # Scoped DNS (in lower section; do not truncate with head)
netstat -rn -f inet | grep -E "^default"     # Scoped default route
ipconfig getsummary feth0 | grep -E "State|IsPublished|Router"
sudo ipconfig set feth0 NONE                 # Teardown

# Verify kernel SIOCAIFADDR address assignment on feth (expected: succeeds)
sudo ifconfig feth9 create
sudo ifconfig feth9 inet 10.99.99.1 netmask 255.255.255.0 up
netstat -rn -f inet | grep 10.99.99
sudo ifconfig feth9 destroy

# Check whether Homebrew service DSL supports MachServices (expected: no matches)
grep -i machservice /opt/homebrew/Library/Homebrew/service.rb
```

Note: The trailing `!` on interface routes in `netstat -rn` output indicates a scoped route (`RTF_IFSCOPE`), **not an error** — standard `en0` and `bridge` routes carry it as well.


