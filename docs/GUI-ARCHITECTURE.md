# GUI Architecture Notes

This document records **what the TetherKitNext GUI implements, why it is structured that way, and which invariants must not be broken**.

Companion reading: [GUI-SPIKE.md](GUI-SPIKE.md) documents the initial feasibility spike (especially the three approaches that were ruled out); this document describes the production architecture and its constraints. **Read both before modifying GUI or helper code.**

## 0. TetherKitNext Changes (Read First — Supersedes Legacy Install/Trust Sections Below)

Sections 1–6 below preserve the historical design record from upstream. The following parts have changed in TetherKitNext:

| Area | Upstream | Now |
|---|---|---|
| Daemon install | `AuthorizationExecuteWithPrivileges` (via `dlsym`) runs the helper with `--install`, which calls `setuid(0)` and runs `install-helper.sh` to copy files into `/Library/PrivilegedHelperTools` + `/Library/LaunchDaemons` | `SMAppService.daemon(plistName: "com.tetherkitnext.helperd.plist")`. `launchd` runs `Contents/MacOS/tetherkitnext-helper` **in place** from the signed bundle; the user approves it once in Login Items. No files are copied; `InstallerMode.swift` and `install-helper.sh` are gone |
| Label / Mach service | `com.tetherkitnext.helper` | `com.tetherkitnext.helperd` (distinct label so it never collides with a still-loaded legacy job). The new daemon boots out and deletes the legacy install on first start (`LegacyHelper`) |
| Who may connect | Anyone; security relies only on per-call admin authorization | Team-signed builds: `setCodeSigningRequirement` on both ends (`CodeSigning.swift`, Team ID read from the running binary). Per-call authorization is kept as a second layer. Ad-hoc builds fall back to the authorization-only model |
| Protocol revision | 3 | 5 (`SMAppService` daemon + `setCommandLineToolInstalled` + Touch ID empty-authorization support) |
| Bundle layout | `Contents/Library/HelperTools/` payload | `MacOS/{TetherKitNext,tetherkitnext-cli,tetherkitnext-helper}`, `Frameworks/{libtetherkitnext.0,libusb-1.0.0}.dylib`, `Library/LaunchDaemons/com.tetherkitnext.helperd.plist`, `Resources/Licenses/` |
| CLI on PATH | Homebrew formula | Daemon-managed symlink `/usr/local/bin/tetherkitnext-cli` → the CLI inside its own bundle (`CommandLineToolLink`) |
| Architectures | arm64, Homebrew libusb | Universal (`arm64` + `x86_64`); libusb 1.0.30 built from a hash-pinned tarball (`scripts/build-libusb.sh`) |
| Swift | tools 5.9, Swift 5 mode | tools 6.2, Swift 6 language mode |
| Layout | Single dashboard under a 700 pt height budget | `NavigationSplitView`: Overview / Device / Network / Activity / Settings; Connect button in toolbar |
| Dock icon | Activation policy set in SwiftUI `onDisappear` (not reliably called when a `Window` scene closes) | `AppDelegate` derives it from `NSWindow` notifications |
| Finder alias | Created on launch (for Homebrew Cellar installs) | Removed |

Constraints that still hold: UTF-8 boundary truncation in the C ABI, enum-order `static_assert`s, `.id(model.languageRevision)` for live language switching, and never blocking an XPC queue.

---

## 1. Process and Trust Model

```
┌───────────────────────────────┐         ┌────────────────────────────────┐
│  TetherKitNext.app  (uid 501) │         │  tetherkitnext-helper  (uid 0) │
│                               │   XPC   │                                │
│  · SwiftUI interface          │ ──────► │  · Owns the RNDIS session      │
│  · Prompts for auth token     │         │  · Creates/destroys feth + BPF │
│  · Polls status to refresh UI │ ◄────── │  · Configures IP/routes        │
│  · Never touches USB or NICs  │         │  · Verifies caller credentials │
└───────────────────────────────┘         └────────────────────────────────┘
                                                        ▲
                                                        │ On-demand (MachServices)
                                                  ┌─────┴──────┐
                                                  │  launchd   │
                                                  └────────────┘
```

**The helper's root privileges come from `launchd`, independent of user prompts.** Because the helper starts as `uid 0`, it must authenticate callers itself: every privileged XPC method requires either a verified Team ID code-signing match + local `admin` group membership (when confirmed via Touch ID in team-signed builds) or an externalized `AuthorizationRef` token verified by the helper.

> **Key principle: Authorization ≠ root.**
> `AuthorizationCopyRights` does not change the calling process's `uid` or `euid`. It produces a credential token proving the user authorized an action at a point in time, which the unprivileged app passes to the already-root helper for verification.

---

## 2. Directory Layout

```
include/tetherkitnext/capi/tetherkitnext_c.h   Sole extern "C" boundary in the repository
src/capi/                                      C ABI implementation
├── capi_support.{h,cc}                        Fixed-buffer UTF-8 copy, error translation, NIC name validation
├── core_foundation_support.{h,cc}             Minimal RAII wrappers for CF / SCDynamicStore
├── environment.cc                             Version, environment preflight, device enumeration (non-root)
├── log_ring.cc                                Log ring buffer
├── managed_network_service.{h,cc}             Persistent SCNetworkService registration for DHCP
├── net_config.cc                              DHCP / static IP / active state query
├── orphan_cleanup.cc                          Persistent feth registry and orphan cleanup
├── process_runner.{h,cc}                      posix_spawn execution of external tools (no shell)
└── session.cc                                 Session lifecycle, status snapshots, event polling

gui/
├── Package.swift                              SwiftPM manifest
├── Sources/
│   ├── CTetherKitNext/                        Module map for the C ABI (symlinked header)
│   ├── TetherKitNextIPC/                      Shared between App & helper: XPC protocol, models, auth, i18n
│   ├── TetherKitNextCore/                     Swift wrapper over the C ABI
│   ├── TetherKitNextHelper/                   Privileged helper daemon
│   └── TetherKitNextApp/                      SwiftUI application
├── Tests/TetherKitNextIPCTests/               Auth token lifetime, code signing, and i18n placeholder tests
├── Resources/                                 App-Info.plist, LaunchDaemon plist, AppIcon
└── Scripts/                                   GUI packaging and helper uninstallation scripts
```

---

## 3. Data Flow

| Direction | Mechanism | Cadence |
|---|---|---|
| Library → helper: status | `tk_session_status_get` snapshot copy | On demand |
| Library → helper: events | `tk_session_poll_events` ring buffer drain | Piggybacked on status poll |
| Library → helper: logs | `tk_drain_logs` ring buffer drain | Every `drainFeed` call |
| Helper → App | XPC with JSON-encoded `Codable` payloads | 500 ms (active/visible) / 2 s (background idle) |

**Zero callbacks cross the language boundary.** Low-level callbacks originate on libusb event threads and RNDIS control threads; marshaling across C++/Swift boundaries risks re-entrancy deadlocks (e.g., calling `Stop()` inside a callback would attempt to join the control thread from itself). Instead, events and logs are queued inside `libtetherkitnext` and polled by the host.

---

## 4. Critical Implementation Constraints (Do Not Break)

### 4.1 C ABI

- **No cross-boundary ownership transfer.** All outputs are written into caller-allocated fixed-size structs. The sole opaque pointer is `tk_session_t*`, with explicit create/destroy pairing.
- **Fixed-size string truncation must land on a valid UTF-8 code-point boundary.** Slicing mid-character in multi-byte UTF-8 text causes Swift's `String` decoding to corrupt or replace strings. Both C (`CopyText` in `capi_support.h`) and Swift (`CInterop.swift`) enforce safe UTF-8 boundary handling.
- **C enums and C++ enums are cast directly by ordinal position**, locked down by `static_assert` in `session.cc`. Never insert or reorder enum values without updating both sides.
- **Member declaration order in `tk_session` matters**: `events` must be declared before `runtime` so that `runtime` joins its control thread before `events` is destroyed, preventing use-after-free.

### 4.2 `Runtime` Threading Model

`Runtime` owns its dedicated control thread where **the startup sequence, keepalive loop, and teardown sequence all run**. This guarantees non-blocking `Start()` and enforces `StateMachine`'s single-thread invariant structurally.

Host threads read state **exclusively** through `Snapshot()` and never touch internal component pointers directly, avoiding data races against `Stop()`.

`RequestStop()` writes a single atomic flag (async-signal-safe), whereas `Stop()` also notifies a condition variable so clicking **Disconnect** in the UI responds immediately.

### 4.3 Network Configuration

- **Always configure addresses via `ipconfig` / `SCNetworkService`, never raw `SIOCAIFADDR` alone.** Raw socket ioctls are recognized by the kernel but ignored by `configd` (resulting in no network service, no scoped DNS, and no default route).
- **External tools (`/usr/sbin/ipconfig`, `/sbin/route`) are executed via `posix_spawn` with an explicit `argv` array and a sanitized environment, never via `/bin/sh`.**
- **Drain stdout/stderr pipes before calling `waitpid`** to avoid classic 64 KiB pipe buffer deadlocks (`process_runner.cc`).
- **Interface names are strictly validated to match `feth<digits>`**, preventing accidental modification of primary interfaces such as `en0`.
- **`SCDynamicStore` handles must be long-lived at the process level**, because dynamic store entries are tied to the session that created them and vanish if a temporary handle is released.

### 4.4 Authorization Verification (`TetherKitNextIPC/Authorization.swift`)

Four security-critical invariants are enforced in `Authorization.swift`:

1. **Helper verification must never pass `.interactionAllowed`** — the root daemon has no UI session and must never allow arbitrary processes to trigger system authorization prompts.
2. **`AuthorizationFree` in the helper must never pass `.destroyRights`** — the credential belongs to the App; destroying rights in the helper would invalidate the App's cached token.
3. **Read-only probe methods do not require authorization**, allowing the UI to distinguish between "helper not running" and "authorization denied".
4. **`AuthorizationMakeExternalForm` produces a 32-byte handle referencing `securityd`'s internal state, not a self-contained token.** If the App releases its `AuthorizationRef` before the XPC call completes, the helper receives `errAuthorizationDenied (-60005)`. `AuthorizationToken` and `AuthorizationBroker.withAuthorization` use `withExtendedLifetime` to keep the reference alive across the XPC round-trip.

**Tokens must be cached and reused (`system.privilege.admin` attributes from `security authorizationdb read`):**

| Attribute | Value | Meaning |
|---|---|---|
| `shared` | `false` | Credentials are **not shared across `AuthorizationRef` instances**; creating a new `AuthorizationRef` always prompts again |
| `timeout` | `300` | Credentials on the **same** `AuthorizationRef` remain valid for 5 minutes |

Therefore, the App caches `AuthorizationToken` across operations for up to 5 minutes (`connect` → automatic post-connect `applyNetwork` → `disconnect` do not prompt again within the window).

### 4.5 Touch ID (Since 1.1.0)

`AuthorizationCopyRights` system prompts only accept an administrator password and do not support Touch ID. In Developer ID team-signed builds, both ends of the XPC connection enforce a mutual code-signing requirement (`CodeSigning.swift`), allowing safe use of `LocalAuthentication`:

- The App confirms user presence via `LAContext.evaluatePolicy(.deviceOwnerAuthentication)` (Touch ID / Apple Watch / password fallback) and sends an empty authorization `Data` payload (`TetherKitNextApp/UserPresence.swift`).
- The helper accepts empty authorization data **only** when both conditions hold (`HelperService.authorize`): the XPC client is pinned to our team-signed App (`CodeSigning.clientRequirement != nil`) **and** the caller's `effectiveUserIdentifier` belongs to the local `admin` group.
- Otherwise, the helper rejects the request as an authorization failure and the App falls back to the standard administrator password dialog.
- Ad-hoc / unsigned development builds have no Team ID (`UserPresence.isAvailable == false`) and always use the `AuthorizationRef` password flow.
- Each confirmation is cached for a 5-minute grace period (`UserPresence.gracePeriod`), so automatic post-connect network configuration does not prompt a second time.

### 4.6 XPC Protocol Revision

`HelperConstants.protocolRevision` (currently **5**) is incremented whenever `TetherKitNextHelperProtocol` changes and is embedded in the `helperVersion` response so the App can detect version mismatches immediately upon connecting and prompt the user to restart/update the background component.

### 4.7 Swift UI & XPC Client Constraints

- **Both the XPC error handler and reply closure can fire** if a connection drops after a request is sent; `HelperClient` wraps every call in `ContinuationGuard` so a `CheckedContinuation` is never resumed twice.
- **Drop the cached `NSXPCConnection` on invalidation/interruption** so subsequent calls reconnect cleanly.
- **All potentially slow operations in the helper reply asynchronously** (USB handshake, DHCP lease acquisition) so they never block status polling on the XPC queue.
- **Throughput calculations divide byte deltas by the library's monotonic timestamp delta**, not the nominal UI timer interval.
- **Fixed-size C `char` arrays import into Swift as tuples**; always decode them via `CInterop.swift` by scanning up to the first `NUL` or the buffer bound rather than `String(cString:)`.
- **Adaptive polling**: 500 ms when a session is active/starting/stopping or the main window is visible; 2 seconds when idle in the background.

### 4.7b Interface Localization

Localization tables are compiled directly into the binaries (`TetherKitNextIPC/LocalizedStrings.swift` and `include/tetherkitnext/common/messages.def`) rather than `.lproj` bundles because the helper daemon needs self-contained localized messages without external bundle dependencies.

Switching languages in `AppModel.applyLanguage` updates three places atomically:

| Target | Call | Effect if Omitted |
|---|---|---|
| Swift string table | `L10n.apply(_:)` | UI labels do not change |
| `libtetherkitnext` (in-app) | `TetherKitNextLibrary.setLanguage(_:)` → `tk_set_language` | In-process library messages remain in the old language |
| Helper daemon | XPC `setLanguage(_:)` | Daemon status messages and session logs remain in the old language |

### 4.8 Packaging (`build-gui.sh` / `make-dmg.sh`)

- **Always copy dylibs with `cp -a` rather than `install`** to preserve versioned symlinks (`libtetherkitnext.dylib` → `libtetherkitnext.0.dylib`).
- **Always re-sign binaries after modifying Mach-O headers with `install_name_tool`**, otherwise macOS on Apple Silicon terminates the process with `Killed: 9` (`SIGKILL` due to invalid code signature).
- **Bundle `libusb-1.0.0.dylib` inside `Contents/Frameworks/`** and sign it with the same identity so Hardened Runtime library validation succeeds.
- **Strip build-directory `@rpath` entries** from release binaries so dyld always loads the bundled Frameworks rather than development paths on the build machine.

### 4.9 In-App Privileged Helper Installation (`SMAppService` / Historical AEWP)

> **Current (TetherKitNext 1.0+)**: `HelperInstaller.swift` manages the helper daemon via `SMAppService.daemon(plistName: "com.tetherkitnext.helperd.plist")` (see Section 0). Historical details below describe the earlier AEWP-based installer for reference.

In the pre-1.0 AEWP design:
- `AuthorizationExecuteWithPrivileges` (`dlsym`-resolved from `Security.framework`) executed a tiny C trampoline (`install-trampoline.c`) that promoted `euid=0` to real `uid/gid=0` via `setuid(0)` / `setgid(0)`, redirected `stderr` into `stdout` (`dup2`), and `exec`'d `install-helper.sh`.
- Success was verified not via process exit code (which AEWP does not expose), but by reading `stdout` to `EOF` and then performing an XPC round-trip to confirm a matching helper version was reachable.
- Uninstallation ran `--uninstall` → `uninstall-helper.sh` and confirmed XPC could no longer connect.

### 4.10 Update Checker (`UpdateChecker.swift`)

`UpdateChecker.swift` queries GitHub's `releases/latest` API and performs semantic version comparison against `CFBundleShortVersionString` in `Info.plist` (single-sourced from `CMakeLists.txt`). It is **intentionally check-only** (it opens the release page rather than self-replacing the `.app` bundle in place):

- **Two entry points**: manual "Check for Updates…" in the application menu (presents a result dialog with the release page link), and an automatic background check at most once per 24 hours that quietly surfaces a non-intrusive badge when a newer release is available.
- **Privacy**: only queries the public GitHub Releases API (`https://api.github.com/repos/AA-EION/TetherKitNext/releases/latest`) and fails silently when offline. Can be disabled completely via:
  ```bash
  defaults write com.tetherkitnext.app updateCheckDisabled -bool YES
  ```
- Bare `swift run` executables have no `Info.plist` version string and automatically skip the check.
- Every release must be tagged `vX.Y.Z` and published as a GitHub Release so `releases/latest` resolves it.

---

## 5. Implemented Features & Known Limitations

### Implemented

| Capability | Location |
|---|---|
| Environment preflight (root, `feth` sysctls, MTU cap) | `tk_check_environment` |
| USB device enumeration + vendor/product/serial strings | `tk_list_devices` |
| Session lifecycle (non-blocking start, status snapshots, events) | `tk_session_*` |
| Log ring capture and polling | `tk_drain_logs` |
| DHCP / Static IPv4 / Teardown | `tk_net_apply`, `tk_net_clear` |
| Live active network state readback (IP, gateway, DNS, primary default route) | `tk_net_query` |
| Orphan `feth` interface cleanup | `tk_cleanup_orphan_interfaces` |
| Privileged helper daemon (`SMAppService`) + Team ID & authorization checks | `gui/Sources/TetherKitNextHelper` |
| In-app helper registration / update / uninstallation | `gui/Sources/TetherKitNextApp/HelperInstaller.swift` |
| Update checker (check-only, daily auto-check + manual menu item) | `gui/Sources/TetherKitNextApp/UpdateChecker.swift` |
| Finder alias / `/Applications` placement check | `gui/Sources/TetherKitNextApp` |
| SwiftUI dashboard (Status, Devices, Network, Throughput, Activity log, Settings) | `gui/Sources/TetherKitNextApp` |
| Live menu bar throughput indicator + background (menu-bar-only) mode | `gui/Sources/TetherKitNextApp/Views/MenuBarPanel.swift` |
| Bilingual UI (Chinese / English, runtime-switchable across UI, C++ library, and helper) | `gui/Sources/TetherKitNextIPC/Localization.swift`, `tk_set_language` |

### Known Limitations

| Item | Notes |
|---|---|
| **Static IP DNS registration** | `IPConfiguration` natively publishes DNS only in DHCP mode. In static IP mode, `tk_net_query` always reads back the actual system resolver configuration (`SCDynamicStore` / `scutil --dns`) so the UI reflects real state. |
| **End-to-end DHCP verified on real hardware** | Verified with Android USB tethering (2026-07-28): plug in → Connect in App → `feth0` acquires DHCP lease on phone subnet → bidirectional ping → `iperf3` reaches ~326 Mbps RX / ~302 Mbps TX. Session stays active in the helper even if the GUI app quits (background mode by design). |
| **AuthorizationServices vs. Touch ID** | `AuthorizationCopyRights` prompts only for an admin password (a macOS system API limitation; see Section 4.5). Since 1.1.0, signed builds use `LAContext` for Touch ID confirmation before XPC dispatch, with a 5-minute token grace window. |
| **Hotplug** | `Context::SupportsHotplug()` is available in the USB layer, while the GUI currently refreshes device enumeration every 2 seconds. |
| **IPv6** | Network configuration in the GUI currently manages IPv4 (`DHCP` / static IPv4); IPv6 link-local works automatically via separate MAC addresses on `feth0` and `feth1`. |
| **Single active session** | The helper daemon manages one active RNDIS session at a time. |
| **App icon history** | Originally used the system default icon during initial spike; full `.icns` asset set added in commit #24 and updated for TetherKitNext 1.0 in commit #42. |
| **Supported UI languages** | Chinese (`zh-Hans`) and English (`en`) are compiled into the binary tables (`Localization.swift` and `messages.def`). Adding a third language requires extending the `Language` enum and exhaustive `switch` / X-macro tables. |

---

## 6. Building and Running

```bash
# 1. Build C++ core and shared library (produces libtetherkitnext.dylib)
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 2. Build and bundle GUI (.app bundle via ./gui/Scripts/build-gui.sh)
cmake --build build --target gui

# 3. Launch the app. On first launch, enable the privileged background component
#    in System Settings › General › Login Items & Extensions (see Section 0 & 4.9).
open dist/TetherKitNext.app
```

During development, you can also build and run the SwiftPM package directly:

```bash
export TETHERKITNEXT_LIB_DIR="$PWD/build/lib"
swift build --package-path gui
swift test  --package-path gui     # Runs TetherKitNextIPC unit tests
```

To uninstall the helper daemon and clean up state from the terminal:

```bash
sudo ./gui/Scripts/uninstall-helper.sh
```

Troubleshooting quick reference:

| Symptom | Where to Check First |
|---|---|
| UI stays on "Enable Background Component" | Check `launchctl print system/com.tetherkitnext.helperd` (or legacy `system/com.tetherkitnext.helper`) and ensure the app is in `/Applications` and enabled in *System Settings › General › Login Items & Extensions*. |
| Helper fails to start | Check `/var/log/tetherkitnext-helper.log` (or unified logging for `com.tetherkitnext.helperd`). |
| Connection fails | Check the **Activity** log panel in the app (supports one-click copy). |
| App still links against Homebrew `libusb` | Inspect `otool -L dist/TetherKitNext.app/Contents/Frameworks/libtetherkitnext.0.*.dylib`. |
| Clicking "Connect" reports `errAuthorizationExternalizeNotAllowed (-60005)` | The App released `AuthorizationRef` prematurely before the helper finished `CreateFromExternalForm`; see Section 4.4 rule 4. |
| UI shows "Background Component Needs Update" | The app and running helper have mismatched `protocolRevision` or `buildId`; click **Update Background Component** in the app. |
| Clicking "Install Helper" reports missing payload | Running a bare `swift run` executable instead of the `.app` bundle assembled by `build-gui.sh`. |
| Authorization prompt asks for password instead of Touch ID | `AuthorizationCopyRights` only supports password entry on ad-hoc builds; signed builds (1.1.0+) use `LAContext` for Touch ID (see Section 4.5). |
| Re-plugging device and clicking "Connect" says session is already running | Running an older helper daemon that did not clear `failed`/`stopped` sessions; update/restart the background component. |
| Double-clicking Finder icon does nothing in Menu Bar Only mode | Known trade-off when activation policy is `.accessory` (see `MainWindowRoot`); open the main window from the menu bar panel (`Open TetherKitNext`). |


