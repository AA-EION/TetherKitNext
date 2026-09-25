# TetherKitNext security audit

Date: 2026-09-25 · Base: upstream `XiaoMiku01/TetherKit` v0.1.5 (`1ded051`)

Scope: every source file in this repository — the C++ driver (`src/`,
`include/`), the C ABI, the Swift app, the root daemon, the build scripts and
the CI workflows — reviewed for backdoors, memory safety, privilege
escalation, race conditions, and conformance with Apple's requirements for
Developer ID signing and notarization.

## Summary

**No backdoor or malicious code was found.**

| Check | Result |
|---|---|
| Outbound network connections | One: the optional, read-only GitHub release check (`UpdateChecker.swift`). It downloads nothing and runs nothing. |
| Process execution | `/usr/sbin/ipconfig` and `/sbin/route` with absolute paths through `posix_spawn` and an argv array (no shell). Arguments are validated interface names (`feth<N>`) and IPv4 literals. The legacy installer's `execv("/bin/bash")` has been removed. |
| Dynamic code loading | Upstream: `dlsym("AuthorizationExecuteWithPrivileges")` (removed). Now only `dlsym("_SCNetworkInterfaceCreateWithBSDName")`, a documented, optional lookup of an Apple SPI. |
| Obfuscation, encoded payloads, hidden persistence | None. The only persistence is the LaunchDaemon, which the user approves and can see in System Settings › Login Items. |
| Vendored code | `third_party/doctest/doctest.h` is byte-identical to upstream doctest v2.4.12 (SHA-256 `94029a7d…0ced`). |
| Commit authors | `xiaomiku01` (upstream) and one contribution by `LefterisEvan` (PR #4, reviewed below). |

Several real defects were found and fixed. They are listed below by severity.

## Findings and fixes

### High

**H1. The root daemon accepted XPC connections from any local process.**
`HelperListenerDelegate` accepted every connection. Privileged calls did
require an admin `AuthorizationRef`, but the probe methods (device list, logs
that include device serial numbers, status) and `setLanguage` were open to any
local user. The authorization-only model was also a deliberate workaround for
source builds, which can't be code-signed consistently.
*Fix:* team-signed builds now call `setCodeSigningRequirement` on both sides.
The daemon only accepts `com.tetherkitnext.app` signed by the same Team ID, and
the app only talks to `com.tetherkitnext.helperd` signed by the same Team ID. The
check runs against the peer's audit token, so PID reuse can't spoof it. The
Team ID is read at runtime from the binary's own signature
(`CodeSigning.swift`). The per-call admin authorization check is kept as a
second layer.

**H2. The installer relied on a deprecated privilege-escalation API.**
In-app install ran the helper binary through `AuthorizationExecuteWithPrivileges`,
which has been deprecated since macOS 10.7 and was looked up with `dlsym` so it
would compile. The helper then called `setuid(0)` and ran a bash script that
copied root-owned binaries into `/Library`. The script and payload were read
from the app bundle, which is user-writable in `~/Downloads` or when
`/Applications` is owned by the user. That leaves a window between
authorization and execution in which the payload can be swapped
(time-of-check to time-of-use).
*Fix:* replaced by `SMAppService.daemon`. launchd runs the signed daemon in
place from the signed, notarized bundle. Nothing is copied, the user approves
the daemon in Login Items, and deleting the app removes it. The
`--install`/`--uninstall` setuid mode no longer exists; CI checks this.

**H3. Idle sessions pinned a CPU core at 100%** (upstream issue #5; this is a
resource-exhaustion defect rather than a vulnerability). The RX injector spun
on `std::this_thread::yield()`.
*Fix:* an eventcount doorbell on `FrameRing`. The consumer parks on a futex,
and producers wake it only when it is parked. A stress test confirmed no lost
wakeups, and idle CPU dropped to about 0.03%. Regression tests were added.

### Medium

**M1. Orphan cleanup could destroy another process's live interfaces.**
At startup, the registry at `/var/run/tetherkitnext-interfaces` was treated as all
orphans. A second daemon instance (for example the legacy daemon plus the new
one during an upgrade) would tear down a running session's `feth` pair and
remove its network service.
*Fix:* each registry entry now records its owner PID. Cleanup skips entries
whose owner is alive, and skips the service cleanup while any such owner
exists.

**M2. Upstream PR #4 hard-linked a private SystemConfiguration symbol.** A
macOS release that drops `_SCNetworkInterfaceCreateWithBSDName` would stop
dyld from loading `libtetherkitnext` at all, taking down both the app and the
daemon.
*Fix:* the symbol is resolved with `dlsym`. If it is missing, DHCP falls back
to the transient `ipconfig set` service.

**M3. PR #4 wrote the network preferences without `SCPreferencesLock`.** A
concurrent writer such as System Settings or `networksetup` could interleave
with the read-modify-commit.
*Fix:* the lock is held for every modification.

**M4. The update check opened an unvalidated URL from the network.**
`html_url` from the GitHub API response, and the copy cached in
`UserDefaults`, was passed to `NSWorkspace.open`. A spoofed or tampered
response could open `file://` or custom-scheme URLs.
*Fix:* only `https://github.com/...` URLs are accepted.

### Low

**L1. Root child processes inherited the full environment.** `ipconfig` and
`route` now run with a fixed `PATH` and `LANG=C`.

**L2. The CI ABI gate could never pass.** It searched the log for `创建了 feth`,
a string that disappeared when log messages were localized. It now pins
`TETHERKITNEXT_LANG=en` and matches the current message.

## Areas reviewed with no issue found

* **Parsing untrusted USB data in a root process.** This is the largest attack
  surface: a malicious USB device talking to the daemon. `PacketMessageReader`
  and the control-message decoders do all offset arithmetic in 64-bit and
  check it against a validated `MessageLength`, which is itself checked
  against the received buffer. They reject frames that are too short or too
  long, and don't trust device-reported sizes. `PacketAlignmentFactor` is
  clamped to 7 before `1 << n`, `MaxTransferSize` is clamped in both
  directions, and a zero `MaxPacketsPerMessage` is treated as 1.
* **Argument validation.** IPv4 literals are checked with `inet_pton` and
  interface names are restricted to `feth<N>`, both in the C ABI and again in
  the daemon (`NetworkValidator`).
* **`/var/run` registry.** The file is root-only. Its contents are re-validated
  on every read and treated as untrusted input.
* **Authorization.** The daemon verifies the right without
  `kAuthorizationFlagInteractionAllowed` and never destroys the caller's
  rights.
* **Lock-free rings.** The SPSC ring's acquire/release pairs are correct. The
  new park/wake handshake uses paired seq_cst fences (Dekker). TSan runs in CI.
* **CLI symlink management (new, root).** The target is derived from the
  daemon's own bundle location and is never taken from the client. The
  directory must be a real directory. Only symlinks that are ours or dangling
  are replaced; `symlink`/`unlink` never follow links.

## Apple distribution requirements

| Requirement | Status |
|---|---|
| Developer ID signature on every Mach-O, signed inside-out | `build-gui.sh` |
| Hardened runtime (`--options runtime`), secure timestamp | Enabled whenever a signing identity is configured |
| Entitlements | None needed. The app runs as the user, and the daemon runs as root through launchd. |
| No deprecated privilege APIs | AEWP removed; SMAppService (macOS 13+) is used |
| Library validation | All bundled dylibs are signed by the same team |
| Notarization + stapling | App (zip) → staple → DMG → notarize → staple (`make-dmg.sh`) |
| Private API use | `feth`'s `if_fake` ioctl ABI (the core of the product, gated by the CI ABI check) and one optional SystemConfiguration SPI (resolved at runtime). Both are fine for Developer ID distribution; neither would be allowed on the Mac App Store. |
| Third-party licenses | libusb (LGPL-2.1) is shipped as a separate, replaceable dylib, with its license text in `Contents/Resources/Licenses` |

## Residual risks and recommendations

* **Kernel private ABI.** `if_fake_request` and the `BIOC*` private ioctls can
  change in any macOS release. The CI ABI gate runs on each runner image; keep
  it required.
* **Ad-hoc development builds** have no Team ID to pin, so they fall back to
  the authorization-only XPC model. Only distribute builds signed with a
  Developer ID.
* **Per-operation admin prompts.** These are kept to preserve upstream
  behavior. With XPC identity checks in place, you could relax the rule from
  `system.privilege.admin` to a custom right. That is a product decision and
  hasn't been changed.
