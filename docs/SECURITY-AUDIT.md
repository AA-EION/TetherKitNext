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

**L2. The CI ABI gate could never pass.** It searched the log for a legacy Chinese message ("Created feth"),
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
* **Touch ID instead of the admin dialog (since 1.1.0).** Team-signed builds
  confirm the user with LocalAuthentication and send privileged calls without
  an `AuthorizationRef`. The daemon accepts that only from a connection pinned
  to our signed app *and* from a user in the local `admin` group, so the set
  of people who can act is unchanged. The residual risk moves to the app: a
  bug that skips `UserPresence.confirm` before a privileged call would skip
  the prompt. Keep every privileged call behind `AppModel.authorized`.

---

## Addendum: 2026-11-07 re-scan

Re-scan prompted by the recent run of supply-chain attacks (hijacked
GitHub Action tags, poisoned release tarballs). Base: `2e92841` (1.2.0).

### Dependencies

| Component | Pinned | Latest upstream | Verdict |
|---|---|---|---|
| libusb | 1.0.30 (`scripts/build-libusb.sh`) | **1.0.30** (2026-05-17); no newer release or release candidate | Up to date. The pinned SHA-256 was re-checked against the GitHub release asset: `fea36f34…dbaf`, match. |
| doctest | 2.4.12 (vendored, test-only) | n/a | Not shipped. |

libusb `master` has darwin fixes since 1.0.30 (interface claim/re-claim
bookkeeping after a failed state restore, run-loop reference handling,
parent-session refresh on re-enumeration) and a memory-safety fix in
`libusb_get_port_numbers` (`1110f3a`, a write before the caller's array
when a re-enumerated device's parent chain grows between two passes).
TetherKitNext calls none of `libusb_get_port_numbers`,
`libusb_set_configuration` or `libusb_reset_device`, so none of these is
reachable from our code. **Decision: stay on the signed 1.0.30 release** and
take 1.0.31 when it is tagged, rather than shipping an untagged snapshot.

### CI supply chain (fixed)

**S1. GitHub Actions were referenced by mutable tags** (`actions/checkout@v5`,
…). A moved tag runs new code in jobs that hold the Developer ID certificate
and notary key. All `uses:` are now pinned to full commit SHAs (same major
versions, latest patch: checkout v5.1.0, cache v4.3.0, upload-artifact v4.6.2,
download-artifact v4.3.0), with `.github/dependabot.yml` proposing updates.

**S2. `actions/checkout` persisted the job token in `.git/config`.** No job
pushes with git (the prerelease job uses `GH_TOKEN` via `gh`), so every
checkout now sets `persist-credentials: false`; a compromised build step can
no longer read the token from disk.

Reviewed and fine: workflow-level `permissions: contents: read` with
`contents: write` only on the release/prerelease jobs; no
`pull_request_target`; secrets only reach non-PR events, so fork PRs build
ad-hoc signed; the libusb tarball is hash-verified before use.

### Code (fixed)

**L3. Root child processes inherited every open descriptor.**
`posix_spawn` without `POSIX_SPAWN_CLOEXEC_DEFAULT` passes the daemon's
descriptors that lack `FD_CLOEXEC` (BPF device, routing / `feth` sockets,
libusb's internal pipes) to `ipconfig` and `route`. Those are
Apple binaries, so this was not exploitable on its own, but a root process
should hand a child exactly stdin/stdout/stderr. `process_runner.cc` now sets
`POSIX_SPAWN_CLOEXEC_DEFAULT` and gives the child `/dev/null` as stdin.

### Re-reviewed with no new issue

* RNDIS decoding of device data (`packet_codec.h`, `messages.cc`): 64-bit
  bounds arithmetic, `MessageLength ≥ 44` guarantees forward progress,
  alignment factor and transfer size clamps intact.
* XPC: Team-ID requirement on every connection for signed builds; privileged
  calls re-verify an authorization or the presence path (`admin` group);
  unauthenticated calls are read-only (`queryNetwork` validates the interface
  name in the C ABI).
* CLI symlink: `lstat`/`readlink`/`symlink` never follow links; foreign
  entries are reported, not replaced.
* New in this change: the Advanced → Interfaces page reads `getifaddrs` and
  the `NET_RT_IFLIST2` sysctl in the unprivileged app. It parses kernel data
  with bounds checks (`ifm_msglen` validated against the buffer before each
  step) and sends nothing to the daemon or the network.
