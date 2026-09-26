# AGENTS.md — TetherKitNext Implementation Reference

> This file serves as the **working memory** for AI agents and human maintainers.
> Update the corresponding sections after completing a commit. Read this file before starting any work to avoid repeating known pitfalls.
>
> **Naming**: Starting in 1.0, this project was renamed **TetherKitNext** (maintained by Issen Software Group, forked from `XiaoMiku01/TetherKit`). Identifiers in the historical entries prior to row 42 of Section 5 were batch-renamed alongside the codebase and describe behavior at the time they were written (originally named `TetherKit` / `tetherkit-cli` / `com.tetherkit.*`).

---

## 1. Project Summary

A macOS **user-space** RNDIS driver: communicates with RNDIS devices (such as Android USB tethering) over USB via `libusb`, and reads/writes raw Ethernet frames on the host side via a `feth` virtual Ethernet pair + BPF, exposing the USB device as a native network interface to macOS.

---

## 2. Empirically Verified Environment Facts (**Do Not Re-Verify; Cite Directly**)

Verified on: macOS 26.5.1 (Darwin 25.5.0), Apple Silicon `arm64`, Apple Clang 21.0.0.

### 2.1 Toolchain

| Fact | Conclusion | How Verified |
|---|---|---|
| C++23 on Apple Clang 21 | **Available**: `std::expected`, `std::byteswap`, `std::format`, `jthread`, `stop_token`, `latch`, `counting_semaphore` all compile and run | Compiled and executed |
| `std::expected` / `std::byteswap` under `-std=c++20` | **Unavailable** (C++23 library features). Project therefore requires C++23 | Compile test |
| `std::format` floating-point formatting | Depends on libc++ `std::to_chars`, which carries an availability annotation requiring **deployment target ≥ macOS 13.3** | `-mmacosx-version-min=13.0` fails with `'to_chars' is unavailable: introduced in macOS 13.3` |
| `std::hardware_destructive_interference_size` | **Do not use** on Apple libc++ (either absent or reports 256). Define explicit cache-line constants instead | Compile test |
| **Cache line size** | **128 bytes** (not 64!) `hw.cachelinesize: 128`. False-sharing padding in SPSC rings must align to 128 bytes | `sysctl hw.cachelinesize` |
| L1D cache | 65536 bytes | `sysctl hw.l1dcachesize` |
| CPU | 10 logical cores, including **4 Performance cores** (`hw.perflevel0.logicalcpu = 4`). Data-path threads request P-cores via QoS | `sysctl hw.ncpu hw.perflevel0.logicalcpu` |
| `-mcpu=apple-m1` / `-mcpu=native` | Supported, but **off by default** (see trade-offs in `cmake/Optimizations.cmake`) | Compile test |
| `ninja` | **Not assumed installed**; use default Unix Makefiles generator | `which ninja` |
| CMake | 4.3.3+ | `cmake --version` |

### 2.2 `libusb`

| Fact | Conclusion |
|---|---|
| Version | 1.0.30 (`scripts/build-libusb.sh` for universal release builds, or `/opt/homebrew/opt/libusb` for local dev) |
| `pkg-config` | Available. Header path is `.../include/libusb-1.0` (handled by `FindLibUSB.cmake`) |
| Hotplug | `libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)` returns **1** (supported) |
| Link dependencies | Darwin backend requires `IOKit`, `CoreFoundation`, and `Security` frameworks |
| **Physical USB devices** | **Do not assume a physical RNDIS device is attached when running tests.** All USB logic is abstracted behind mockable interfaces (`MockControlChannel`, `MockDataChannel`) so unit tests and benchmarks run offline |

### 2.3 BPF (Darwin)

| Fact | Conclusion |
|---|---|
| **Zero-copy BPF** | **Does not exist on Darwin.** `net/bpf.h` has **no** `BIOCSETZBUF` / `BIOCGETZMAX` / `BIOCROTZBUF` (FreeBSD has them; Darwin does not). Use classic BPF with a large `BIOCSBLEN` + batched `read()` |
| Available public ioctls | `BIOCGBLEN`(102R) `BIOCSBLEN`(102WR) `BIOCSETF`(103) `BIOCFLUSH`(104) `BIOCPROMISC`(105) `BIOCGDLT`(106) `BIOCGETIF`(107) `BIOCSETIF`(108) `BIOCSRTIMEOUT`(109) `BIOCGRTIMEOUT`(110) `BIOCGSTATS`(111) `BIOCIMMEDIATE`(112) `BIOCVERSION`(113) `BIOCGRSIG`(114) `BIOCSRSIG`(115) `BIOCGHDRCMPLT`(116) `BIOCSHDRCMPLT`(117) `BIOCGSEESENT`(118) `BIOCSSEESENT`(119) `BIOCSDLT`(120) `BIOCGDLTLIST`(121) `BIOCSETFNR`(126) |
| `struct bpf_hdr` | `{ struct BPF_TIMEVAL bh_tstamp; bpf_u_int32 bh_caplen; bpf_u_int32 bh_datalen; u_short bh_hdrlen; }` — iteration **must** advance by `bh_hdrlen`, never `sizeof(bpf_hdr)` |
| Alignment macro | `BPF_ALIGNMENT = sizeof(int32_t) = 4`, `BPF_WORDALIGN(x) = ((x)+3) & ~3` |
| Device nodes | `/dev/bpf0..3` exist initially (Darwin clones additional nodes on demand) |
| **Batch write** | ✅ **Supported (macOS 14+)**: private ioctl `BIOCSBATCHWRITE = _IOW('B',143,int) = 0x8004428f`. Buffer layout mirrors `read()` (contiguous `bpf_hdr + frame` records aligned with `BPF_WORDALIGN`) and requires `BIOCSHDRCMPLT=1`. Unsupported on macOS 13 and earlier → must probe at runtime and fall back to per-frame `write()` |
| Private ioctl numbers | `BIOCSBATCHWRITE=0x8004428f`, `BIOCSNOTSTAMP=0x80044291` (disables per-frame `microtime`), `BIOCSWRITEMAX=0x8004428c`, `BIOCSDIRECTION=0x8004428a`, `BIOCSHEADDROP=0x80044280` (moved to `net/bpf_private.h` in macOS 26, omitted from public SDK) |
| `BIOCSBLEN` cap | `sysctl debug.bpf_bufsize_cap` = **33554432 (32 MiB)**. Oversized values **do not return an error**; the kernel silently clamps the size and writes the actual value back into the `_IOWR` parameter |
| `read()` buffer length | **Must equal `bd_bufsize` exactly**, otherwise `bpfread` immediately returns `EINVAL`. Always allocate using the value written back by `BIOCSBLEN` |
| `BIOCSHDRCMPLT` | **Must be set to 1.** When 0 on `feth`, `bpfwrite` fails with `ENXIO` (`if_fake` cannot handle the `AF_UNSPEC` header-rebuild branch). Batch writes also require `BIOCSHDRCMPLT=1` |
| Max single-frame write length | `BPF_WRITE_LEEWAY = 18`; when `hdrcmplt=1`, total frame length must be ≤ interface MTU + 18 (1518 for MTU=1500) |
| `BIOCSRTIMEOUT` resolution | **10 ms** (`kern.clockrate hz=100`). Passing `{0,0}` blocks indefinitely |
| Optimal read model | `BIOCIMMEDIATE=1` + **dedicated blocking `read()` thread** (no `kqueue`). Wakes on the first arriving packet and drains all packets accumulated in the meantime in a single syscall |
| `/dev/bpf` cloning node | **Does not exist** (`ls /dev/bpf` → No such file). Must iterate `/dev/bpf%d` (`EBUSY` tries next, `ENOENT` stops; opening the last node causes XNU to clone another up to `sysctl debug.bpf_maxdevices = 256`) |
| `access_bpf` group | **Does not exist on macOS** (`/dev/bpf*` is `0600 root:wheel`; root is required) |

### 2.4 `feth` (`if_fake`)

| Fact | Conclusion |
|---|---|
| Availability | Built into macOS (`sysctl net.link.fake.*`) |
| Key sysctls | `net.link.fake.max_mtu = 2048`, `tx_headroom = 32`, `buflet_size = 512`, `qset_cnt = 4`, `link_layer_aggregation_factor = 96` |
| Creation privilege | **Requires root**: `ifconfig feth0 create` as non-root returns `SIOCIFCREATE2: Operation not permitted` |
| `struct ifdrv` | Omitted from public `net/if.h`; declared manually with `#pragma pack(4)`, `sizeof == 40` on LP64. Guarded by `static_assert` (`SIOCSDRVSPEC = 0x8028697b`, `SIOCGDRVSPEC = 0xc028697b`) |
| `net/if_fake_var.h` | Omitted from public SDK. `struct if_fake_request` = `uint64_t reserved[4]` (32 bytes, **must be all zeros**) + 128-byte union = **160 bytes total**. `IF_FAKE_S_CMD_SET_PEER = 1`, `IF_FAKE_G_CMD_GET_PEER = 1` |
| Private ABI stability | **Extremely stable**: `if_fake_var.h` has the exact same MD5 across XNU from `xnu-7195` (macOS 11) through `xnu-12377` (macOS 26); Apple's own `ifconfig fethN peer fethM` uses this ABI |
| `SET_PEER` kernel checks | Five checks (any failure returns `EINVAL`): ① `ifd_len >= 160`; ② `reserved` all zero; ③ peer is also `feth` (`IFT_ETHER`); ④ neither interface already has a peer; ⑤ caller is root |
| Data-flow semantics (`feth_output_common()`) | Frames transmitted by the host out of `feth0` arrive on `feth1` in the **input** direction; frames we `write()` to `feth1`'s BPF fd go out `feth1` and enter `feth0` as **input** to the host IP stack without looping back to `feth1`. Thus a single BPF descriptor on `feth1` handles both RX and TX |
| `BIOCSSEESENT=0` | Sets `bd_direction = BPF_D_IN`, filtering out frames we wrote into `feth1` ourselves. **Omitting this creates a packet loop** |
| `IFF_UP` requirement | `bpfwrite` checks `if ((ifp->if_flags & IFF_UP) == 0) return ENETDOWN` → `feth1` must be UP; `feth0` must also be UP for the host IP stack to process frames |
| **Creation-time sysctl snapshot** | ⚠️ `hwcsum`, `fcs`, `tso_support`, `lro`, `trailer_length`, `separate_frame_header`, and `max_mtu` are snapshotted into the interface inside `feth_clone_create()`. Changing sysctls after creation has no effect. `VerifyFethSysctls()` checks them before creating `feth` |
| Kernel-assigned MAC | `'f','e','t','h', unit>>8, unit&0xff` → `feth0 = 66:65:74:68:00:00` (`0x66` has bit 1 set → valid locally administered unicast MAC) |
| MAC assignment strategy | Assign the device's `OID_802_3_PERMANENT_ADDRESS` to the **host side** (`feth0`) before setting `IFF_UP`; **keep the kernel-assigned MAC on the driver side (`feth1`)** so IPv6 link-local addresses do not collide |
| `BIOCPROMISC` | **Not needed on `feth`**: `feth_output_common` unconditionally delivers frames to the peer and taps BPF without MAC filtering |
| DHCP | GUI registers a **persistent** `SCNetworkService` via `SCPreferences` + `_SCNetworkInterfaceCreateWithBSDName` (resolved via `dlsym`) so NetworkExtension VPNs recognize the route and ServiceOrder can be adjusted. CLI uses `sudo ipconfig set feth0 DHCP` |
| Ceiling note | Community reports note `feth` mbuf exhaustion above ~5–8 Gbps; RNDIS over USB 2.0 HS (~300 Mbps) / USB 3 (~1–2 Gbps) operates far below that ceiling |
| Public socket ioctls (`sys/sockio.h`) | `SIOCSIFFLAGS`(i,16) `SIOCGIFFLAGS`(i,17) `SIOCSIFMTU`(i,52) `SIOCSIFLLADDR`(i,60) `SIOCIFCREATE`(i,120) `SIOCIFDESTROY`(i,121) `SIOCIFCREATE2`(i,122) `SIOCSDRVSPEC`(i,123) `SIOCGDRVSPEC`(i,123) |
| `IFNAMSIZ` | 16 |

### 2.5 Development & CI Environment Constraints

- **Non-root default**: standard test runs execute without root and cannot create `feth` or open `/dev/bpf*`. Root-dependent tests must be opt-in via `TETHERKITNEXT_ROOT_TESTS=1` and skip cleanly otherwise.
- **Offline USB testing**: automated tests must never require a physical USB device attached.

---

## 3. Directory Structure

```
TetherKitNext/
├── AGENTS.md              Agent and maintainer working memory
├── README.md              User-facing documentation (English)
├── CMakeLists.txt         Top-level CMake build configuration
├── .clang-format          Code formatting rules (Google style base, 100 columns)
├── .clang-tidy            Static analysis and naming rules
├── cmake/
│   ├── FindLibUSB.cmake        libusb-1.0 discovery (pkg-config + Homebrew fallback)
│   ├── CompilerWarnings.cmake  Warning flags INTERFACE target
│   ├── Sanitizers.cmake        ASan/UBSan/TSan options
│   └── Optimizations.cmake     Data-path optimization flags and trade-offs
├── include/tetherkitnext/     Public headers (organized by module)
│   ├── common/messages.def     All user-facing localized strings (X-macro, Chinese & English)
│   └── capi/                   C ABI — sole extern "C" boundary in the project
├── src/                   C++ implementation
│   ├── version.cc.in           CMake version template
│   ├── capi/                   C ABI implementation (sessions, network config, logs, orphan cleanup)
│   └── app/                    CLI entry point
├── gui/                   SwiftUI application (SwiftPM package; see docs/GUI-ARCHITECTURE.md)
│   ├── Sources/CTetherKitNext/     Module map for the C ABI (symlinked header)
│   ├── Sources/TetherKitNextIPC/   Shared between App and helper: XPC protocol, models, auth, i18n
│   ├── Sources/TetherKitNextCore/  Swift wrapper around the C ABI
│   ├── Sources/TetherKitNextHelper/  Privileged helper daemon (root, launched via SMAppService / launchd)
│   ├── Sources/TetherKitNextApp/   SwiftUI user interface
│   ├── Resources/              App-Info.plist, LaunchDaemon plist, AppIcon
│   └── Scripts/                Build and helper uninstall scripts
├── tests/                 doctest unit test suite
├── benchmarks/            Lightweight benchmark harness and microbenchmarks
├── third_party/doctest/   Vendored doctest 2.4.12 single-header library
└── docs/                  Design documentation, protocol reference, benchmarks, security audit
```

Dependency direction is **strictly unidirectional**:

```
tk_common  ← (no dependencies)
   ↑
tk_rndis / tk_net / tk_usb  ← tk_common
   ↑
tk_core    ← common + rndis + net + usb
   ↑
tetherkitnext (CLI) ← core
tk_capi (C ABI)     ← core          → packaged as shared library libtetherkitnext
   ↑
gui/ (Swift)        ← libtetherkitnext
```

Swift targets only import `libtetherkitnext` (exporting only `_tk_*` symbols via `cmake/tetherkitnext_exports.txt`).

---

## 4. Coding Conventions (Enforced by `.clang-tidy`)

| Category | Convention | Example |
|---|---|---|
| Namespace | `lower_case` | `tetherkitnext`, `tetherkitnext::rndis` |
| Types (`class`/`struct`/`enum`/alias) | `CamelCase` | `RndisStateMachine`, `BpfLink` |
| Functions & Methods | `CamelCase` | `SendEncapsulatedCommand()` |
| Local variables & parameters | `lower_case` | `frame_len`, `max_transfer_size` |
| Private / protected member variables | `lower_case_` (trailing underscore) | `handle_`, `request_id_` |
| Constants (`constexpr` / enum values) | `k` + `CamelCase` | `kMaxFrameSize`, `kCacheLineSize` |
| Macros | `UPPER_CASE` | |
| Filenames | `snake_case`, `.cc` and `.h` | `spsc_ring.h`, `bpf_link.cc` |

Error-handling tiers:

- **Initialization / Control path**: `std::expected<T, Error>`, carrying `errno`, `libusb` error codes, RNDIS status codes, and localized context strings.
- **Data hot path**: **Never** return `std::expected`, never throw, never allocate heap memory. Use return counts + atomic statistics counters.

### User-Facing Strings and Localization (Chinese / English)

**Never hardcode user-facing string literals in source code**; all user-visible text goes through the localization tables:

| Aspect | C++ (Library + CLI) | Swift (GUI + Helper) |
|---|---|---|
| String table | `include/tetherkitnext/common/messages.def` (X-macro) | `gui/Sources/TetherKitNextIPC/LocalizedStrings.swift` (exhaustive `switch`) |
| Parameterized lookup | `Tr(Msg::kFoo, args...)` (`std::format` syntax) | `L(.foo, args...)` (`String(format:)` printf syntax) |
| Logging | `TETHERKITNEXT_INFO_TR(Msg::kFoo, ...)` macros | — |
| Unparameterized | `Text(Msg::kFoo)` returns `string_view` (zero allocation) | `L10n.text(.foo)` |
| Missing translation guard | Static array bounds + `common.i18n` test | **Compile-time error** (exhaustive `switch`) |
| Placeholder mismatch guard | `common.i18n` validates argument indices & types | `LocalizationTests` validates positions & types |

Three mandatory rules:

1. **Forbidden on hot paths.** `Tr()` and `L()` allocate and format; only use them on initialization and control paths. Use `Text(Msg::kFoo)` (`noexcept` table lookup) on hot paths.
2. **Placeholders in both languages must match in count, index, and type.** Use positional arguments (`{0}`/`{1}` in C++, `%1$@`/`%2$ld` in Swift) when word order differs.
3. **In Swift, always format integers with `%ld` and cast to `Int` at call sites.**

**Switching languages in the GUI must update all three targets together (`AppModel.applyLanguage`)**: Swift string table (`L10n.apply`), `libtetherkitnext` (`tk_set_language`), and the helper daemon (`setLanguage` over XPC).

---

## 5. Implementation History

Legend: `✅ Completed` `🚧 In Progress` `⬜ Not Started`

| # | Commit | Status | Notes |
|---|---|---|---|
| 1 | `chore: initialize project scaffold and build system` | ✅ | CMake + clang-format/tidy + vendored doctest + version injection |
| 2 | `feat(common): infrastructure layer` | ✅ | Error / logging / byte order / lock-free rings / stats / thread QoS |
| 3 | `feat(bench): benchmark harness and microbenchmarks` | ✅ | Built-in harness outputting Markdown to `docs/` |
| 4 | `feat(rndis): RNDIS protocol layer` | ✅ | Constants + control message codec + packet codec |
| 5 | `feat(net): feth virtual NIC and BPF link layer` | ✅ | Includes private ABI declarations and `LoopbackLink` |
| 6 | `perf(common): lock-free ring batch publishing` | ✅ | 2.4× speedup for 1514B, 5.0× for 64B |
| 7 | `feat(usb): USB transport layer` | ✅ | Device discovery / claim / control channel / async transfer pool |
| 8 | `feat(rndis): RNDIS state machine` + CTest fix | ✅ | Fixed quoted `--test-suite` CTest bug |
| 9 | `feat(core): data-path bridge` | ✅ | 3-thread model + backpressure + stats |
| 10 | `feat(app): runtime orchestration and CLI` | ✅ | Startup/teardown order, signal handling, CLI |
| 11 | `docs: design doc, protocol reference, performance guide` | ✅ | |
| 12 | `feat(capi): C ABI boundary and non-root capabilities` | ✅ | Version / preflight / device list; log ring buffer + polling |
| 13 | `refactor(core): self-hosted control thread in Runtime` | ✅ | Non-blocking `Start`, consistent snapshots, event sink |
| 14 | `feat(capi): session lifecycle, status snapshots, event polling` | ✅ | Enum alignment locked down with `static_assert` |
| 15 | `feat(capi): network configuration and orphan cleanup` | ✅ | DHCP / static IP; persistent `feth` registry |
| 16 | `build: output shared library libtetherkitnext` | ✅ | `tk_capi` as `OBJECT` library; exported symbol allowlist |
| 17 | `feat(gui): SwiftPM scaffold, C interop, and XPC protocol` | ✅ | Symlinked header to prevent drift |
| 18 | `feat(gui): privileged helper and authorization verification` | ✅ | LaunchDaemon + `AuthorizationRef` |
| 19 | `feat(gui): SwiftUI design system and main dashboard` | ✅ | Status / device / throughput / log cards |
| 20 | `feat(gui): network configuration UI` | ✅ | DHCP / static IP + active state readback |
| 21 | `build(gui): packaging scripts and GUI architecture docs` | ✅ | |
| 22 | `feat(gui): live menu bar speed and background mode` | ✅ | `MenuBarExtra` + menu-bar-only mode + adaptive polling |
| 23 | `feat(gui): in-app helper installation and uninstallation` | ✅ | Legacy AEWP installer (superseded in #36) |
| 24 | `feat(gui): application icon` | ✅ | Full `.icns` icon set |
| 25 | `feat(gui): update checker (check-only)` | ✅ | GitHub `releases/latest` + semantic version comparison |
| 26 | `feat!: rename CLI to tetherkitnext-cli` | ✅ | |
| 27 | `build(ci): add GUI build to CI` | ✅ | |
| 28 | `chore(release): v0.1.2` | ✅ | |
| 29 | `chore(release): v0.1.3 — preserve device names during active session` | ✅ | `ReconcileDeviceStrings` cache + helper busy check fix |
| 30 | `feat(i18n): full Chinese/English localization` | ✅ | ~390 C++ strings + ~220 Swift strings; `--lang`, `tk_set_language`, XPC rev 3 |
| 31 | `chore(release): v0.1.4` | ✅ | |
| 32 | `feat(gui): prompt to update helper when version mismatches App` | ✅ | Compares library version and build ID |
| 33 | `chore(release): v0.1.5` | ✅ | |
| 34 | `fix(core): park RX injection thread on futex doorbell instead of spinning on yield` | ✅ | Fixes upstream issue #5 (100% idle CPU core → ~0.03%) |
| 35 | `fix(capi): record owner PID in orphan registry; sanitize child env` + harden PR #4 | ✅ | `SCNetworkService` via `dlsym` + `SCPreferencesLock` for VPN compatibility |
| 36 | `feat!: signed universal DMG distribution + SMAppService daemon + bundled CLI` | ✅ | Removed AEWP/setuid installer; bidirectional XPC Team ID verification; universal `libusb`; Intel CI |
| 37 | `feat(gui): Swift 6, Liquid Glass redesign, menu-bar-only mode fix` | ✅ | Sidebar navigation, Settings page, fixed-width menu bar rate, `NSWindow` Dock icon policy |
| 38 | `feat(gui): automatically apply network configuration after connecting (default DHCP)` | ✅ | Reuses auth token grace period |
| 39 | `build(release): prerelease path + user-focused README` | ✅ | |
| 40 | `fix(capi): promote managed service to top of ServiceOrder when routing all traffic` | ✅ | Switches both default route and primary DNS cleanly |
| 41 | `fix(gui): detect stale background daemon across same-version rebuilds via git SHA build ID` | ✅ | |
| 42 | `feat!: rename to TetherKitNext 1.0.0; new icon; guided DMG layout; Issen Software Group` | ✅ | Preserves `HelperConstants.Legacy` (`com.tetherkit.helper`) for clean upgrades |
| 43 | `feat(gui): Touch ID confirmation for privileged actions (1.1.0)` | ✅ | `LAContext` + Team ID XPC pinning + `admin` group check; XPC rev 5 |
| 44 | `fix(gui): make background component restart reliable on first click (1.1.1)` | ✅ | `HelperInstaller.reregister()` waits for `launchd` state transition and retries |

### Current Status (TetherKitNext, 2026-09-25)

- **Distribution**: Signed and notarized universal DMG (`scripts/build-release.sh`, shared across CI and releases). Security audit documented in [docs/SECURITY-AUDIT.md](docs/SECURITY-AUDIT.md).
- **Privileged Daemon**: `SMAppService` with label `com.tetherkitnext.helperd`, executed in-place from the `.app` bundle; automatically cleans up legacy `com.tetherkit.helper` installs on first run. XPC protocol revision 5.
- **CI**: Native builds and tests on `macos-15`, `macos-26`, and `macos-15-intel`, universal `.app` + DMG packaging, Intel hardware DMG installation smoke test, ThreadSanitizer, and `feth` ABI gate.

### Historical Upstream Status (Recorded at v0.1.5)

- **Tests**: 23 CTest suites passing (including `common.i18n` placeholder verification); 17 SwiftPM unit tests passing (`LocalizationTests`, `HelperConstantsTests`, `AuthorizationTests`).
- **Build**: Zero warnings under `-Werror` across CLI, shared library (`libtetherkitnext`), and SwiftUI GUI.
- **Runtime**: `--version`, `--help`, and `--list` work without root; bilingual Chinese/English switching updates GUI views, app menus, C++ library logs, and helper messages immediately at runtime.
- **End-to-End Hardware Benchmarks**: **RX 324–327 Mbps / TX 239–302 Mbps** over USB 2.0 High-Speed (426 Mbps theoretical limit); zero TX drops after `WaitForSendCapacity` fix.

### GUI & Architecture Notes

**Read [docs/GUI-ARCHITECTURE.md](docs/GUI-ARCHITECTURE.md) before modifying the GUI, C ABI, or `Runtime` threading model.** It documents critical invariants that do not fail at compile time if broken (UTF-8 codepoint boundary truncation, `static_assert` enum alignment, `cp -a` symlink preservation, inside-out re-signing after `install_name_tool`, `AuthorizationRef` flags, and `SMAppService` lifecycle transitions).

---

## 6. Verification Checklist (Requires Root or Physical RNDIS Device)

### 6.1 Verified: USB Side (**No Root Required**)

1. **`libusb_open` succeeds without root on macOS.** Enumeration, opening devices, claiming interfaces, and submitting bulk/control/interrupt transfers all work as an unprivileged user (`uid 501`). Root is required **only** for `feth` creation and `/dev/bpf*`.
2. **⚠️ `libusb_kernel_driver_active() == 1` on macOS does NOT imply `libusb_claim_interface` will fail.** Empirically tested on a composite Android RNDIS device:

| Interface Signature | Matched Kernel / DriverKit Driver | `kernel_driver_active` | `libusb_claim_interface` |
|---|---|---|---|
| `02/02/ff` RNDIS Control | **None** | 0 | ✅ Succeeds |
| `0a/00/00` RNDIS Data | `AppleUserECMData` (DriverKit dext) | **1** | ✅ **Still succeeds** |
| `02/02/01` CDC-ACM Control | `AppleUSBACMControl` (kext) | 1 | ❌ `LIBUSB_ERROR_ACCESS` |
| `03/00/00` HID | `AppleUserUSBHostHIDDevice` | 1 | ❌ `LIBUSB_ERROR_ACCESS` |
| `ff/42/01` Vendor Specific | Matched | 1 | ❌ `LIBUSB_ERROR_ACCESS` |

`kernel_driver_active` only indicates that an IOKit driver **matched**, not that it seized exclusive access. `com.apple.DriverKit.AppleUserECM` matches `0a/00/00` without exclusive seizure, so `libusb_claim_interface` succeeds. **Never fail early based on `kernel_driver_active`; always attempt `libusb_claim_interface` directly.**

### 6.2 Verified: `feth` / BPF Side (**Requires Root**)

Reproduce via `sudo TETHERKITNEXT_ROOT_TESTS=1 build/bin/tetherkitnext_tests --test-suite=net.feth`.

3. **`feth` peer pairing private ABI (`SIOCSDRVSPEC` + `struct if_fake_request`)** works on macOS 11–26: create, pair, set MTU/MAC, bring `IFF_UP`, and destroy all pass.
4. **Private BPF ioctls (`BIOCSBATCHWRITE` and `BIOCSNOTSTAMP`)** both succeed on macOS 14–26.
5. **`BIOCSBLEN` effective upper limit is 32 MiB** (`debug.bpf_bufsize_cap`), not `debug.bpf_maxbufsize` (which reports 512 KiB):

| Requested `BIOCSBLEN` | Effective Value Read Back via `BIOCGBLEN` After `BIOCSETIF` |
|---|---|
| 4 KiB / 1 / 4 / 8 / 16 / 32 MiB | Applied verbatim |
| 64 MiB, 2 GiB | Silently clamped to **32 MiB** (`0x2000000`) |

6. **BPF `write()` on driver-side `feth1` delivers frames into the host IP stack on `feth0`** (verified via closed-loop ARP request/reply test in `net.feth`), and `BIOCSSEESENT=0` prevents self-loopback.
7. **End-to-end RNDIS handshake → `feth` pair creation → BPF attachment → graceful `SIGTERM` teardown** leaves zero residual `feth` interfaces.

### 6.3 Completed & Open Verification Items

Completed:

- [x] End-to-end throughput → RX 324–327 Mbps / TX 239–302 Mbps (see Section 6.4).
- [x] Closed-loop ARP round-trip test in `net.feth` (`TETHERKITNEXT_ROOT_TESTS=1`).
- [x] Android RNDIS aggregation parameters (`MaxPacketsPerMessage=10`, `MaxTransferSize=15800`).
- [x] Root cause and fix of high-load TX frame drops (see Section 6.5).
- [x] Graceful `Ctrl-C` / `SIGTERM` shutdown with `WaitForSendCapacity` verified in a real terminal.

Open items for future investigation:

- [ ] Bimodal TCP TX throughput (~240 vs. ~300 Mbps stable modes, both with 0 retransmissions).
- [ ] Bidirectional RTT inflation (0.4 ms idle → 16.6 ms under full RX load).
- [ ] Static IPv4 DNS adoption across macOS network transitions (DHCP path is verified).

### 6.4 Verified: Real-Device End-to-End (Android USB Tethering)

**Device**: vivo iQOO Z10x (`2d95:600b`, Wireless RNDIS `e0/01/03`, Android 15, USB 2.0 High-Speed). Negotiated parameters:

```
RNDIS negotiation complete: version 1.0, MTU 1500, device aggregation limit 15800 bytes / 10 packets, TX alignment 1 byte
```

(`MaxPacketsPerMessage=10`, `MaxTransferSize=15800`, `PacketAlignmentFactor=0` — 10× larger than mainline Linux gadget's `1 packet / 1580 bytes`). Note that Android randomizes the RNDIS MAC address on every session, so never cache the device MAC as a persistent identifier.

### 6.5 Fixed: High-Load TX Frame Drops

**Root cause**: `Bridge::RunTransmitExtractor` immediately discarded remaining frames in a batch when the 4-slot USB bulk OUT pool was full instead of waiting a few hundred microseconds for a transfer completion.

**Fix**: `SendFrames` returns `SendOutcome{consumed, sent_frames, sent_bytes, skipped}` and the bridge calls `WaitForSendCapacity(50ms)` when `consumed == 0`, delegating burst absorption to the 4 MiB kernel BPF buffer.

| Metric | Before Fix | After Fix |
|---|---:|---:|
| TetherKitNext TX frame drops | 1889 | **0** |
| TCP TX retransmissions | 3829 | **0** |
| Bidirectional concurrent TX | 4.8 Mbps | **87 Mbps** |
| Unidirectional TCP RX (regression check) | 324–327 Mbps | 326–327 Mbps |

---

## 7. Known Pitfalls and Safeguards (**Do Not Repeat**)

1. **Cache line size is 128 bytes** (`kCacheLineSize = 128`), not 64.
2. **`std::format` requires `CMAKE_OSX_DEPLOYMENT_TARGET >= 13.3`** (`std::to_chars` availability).
3. **Never use `std::hardware_destructive_interference_size` on Apple libc++.**
4. **BPF has no zero-copy (`BIOCSETZBUF`) on Darwin, but supports batch writes (`BIOCSBATCHWRITE`) on macOS 14+.**
4b. **`BpfLink::WriteFramesBatched` probes `BIOCSBATCHWRITE` at runtime** and automatically falls back to per-frame `write()` on macOS 13.
5. **`BIOCSBLEN` and `BIOCSHDRCMPLT=1` must be called before `BIOCSETIF`.**
6. **Iterate BPF buffers using `bh_hdrlen`, never `sizeof(struct bpf_hdr)`.**
7. **macOS `libusb` does not support `libusb_detach_kernel_driver`.**
8. **Never assume a physical USB device or root privileges in default unit test runs.**
9. **Never quote arguments in CMake `add_test(COMMAND ...)`** — write `--test-suite=${_suite}` without quotes so `doctest` matches the suite name.
10. **Use `FAIL_REGULAR_EXPRESSION` (not `PASS_REGULAR_EXPRESSION`) in CTest**, because `PASS_REGULAR_EXPRESSION` overrides non-zero exit codes.
11. **Time-dependent classes (`PeriodicTimer`) must accept an explicit start timestamp** rather than reading the clock inside the constructor.
12. **⚠️ Synchronous `libusb_interrupt_transfer` blocks forever on macOS**, ignoring `timeout`:
   ```
   Poll → WaitForNotification → do_sync_bulk_transfer
        → sync_transfer_wait_for_completion → handle_events → poll(∞)
   ```
   Darwin sets `USBI_TRANSFER_OS_HANDLES_TIMEOUT` while `submit_interrupt_transfer` calls `ReadPipeAsync` (the no-timeout IOKit variant). Always use a persistent **asynchronous** interrupt transfer (`UsbControlChannel::StartNotificationListener`).
13. **`timeout = 0` in `libusb` on Darwin means block indefinitely**, not non-blocking poll (`kProbeOnlyTimeoutMillis`).
14. **Omitting `BIOCSHDRCMPLT = 1` causes BPF `write()` on `feth` to fail immediately with `ENXIO`** (`Device not configured`), because `if_fake` cannot handle the `AF_UNSPEC` header-rebuild branch.
15. **`osascript ... with administrator privileges` swallows inter-process `SIGTERM` delivery.** Always verify graceful `Ctrl-C` / `SIGTERM` shutdown in a real interactive terminal (`sudo build/bin/tetherkitnext-cli`).
16. **Question "deliberately / on purpose" comments by checking orders of magnitude** (e.g., 256 frames needing 26 transfer slots against a 4-slot pool).
17. **Best-effort USB string descriptors must never overwrite cached valid names with empty strings** when a device is busy (`ReconcileDeviceStrings`), and helper busy checks must track `sessionStarting` + `runState ∈ {starting, running, stopping}`.
18. **SwiftUI does not know when global localization tables change**; bind `.id(model.languageRevision)` on root views and `AppMenuItems`, and use computed properties instead of `static let` for localized strings.
19. **Derive Dock icon visibility from `NSWindow` notifications in `AppDelegate`**, not SwiftUI `onDisappear`.
20. **Pin `TETHERKITNEXT_LANG=en` in CI scripts that grep log output.**
21. **Ad-hoc signed builds cannot register `SMAppService` daemons**; sign with an Apple Development or Developer ID identity (`TETHERKITNEXT_SIGN_IDENTITY`) when testing locally.
22. **Never hard-link private framework SPIs** (`_SCNetworkInterfaceCreateWithBSDName`); always resolve via `dlsym` with a fallback (`managed_network_service.cc`).
23. **Changing the default route alone does not switch DNS**; promote the managed `SCNetworkService` to the top of `ServiceOrder` when routing all traffic through the phone.
24. **The `SMAppService` daemon does not automatically restart when `.app` is replaced**; compare `build <git sha>` build IDs in addition to semantic versions so the UI prompts to restart the background component.
25. **Preserve legacy identifiers (`com.tetherkit.helper`, `libtetherkit*`, `"TetherKit"` service prefix) in cleanup code** during renames so upgrades clean up prior installations.
26. **`dmgbuild` background images fail to render in Finder on macOS 15/26**; `scripts/make-dmg.sh` applies the DMG window layout using Finder via AppleScript on a temporary read-write image.
27. **`SMAppService.unregister()` returning does not mean `launchd` has finished removing the job**; always use `HelperInstaller.reregister()` (which waits for state transitions and retries) to restart the daemon.

---

## 8. Common Commands

```bash
# Configure + build (warnings as errors)
cmake -S . -B build -DTETHERKITNEXT_WARNINGS_AS_ERRORS=ON && cmake --build build -j10

# Run unit tests
ctest --test-dir build --output-on-failure

# Run ThreadSanitizer on lock-free rings
cmake -S . -B build-tsan -DTETHERKITNEXT_ENABLE_TSAN=ON && cmake --build build-tsan -j10 \
  && ctest --test-dir build-tsan --output-on-failure

# Run benchmarks (must use a Release build without sanitizers)
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j10 \
  && ./build-rel/bin/tetherkitnext_bench

# GUI: build, test, and package (requires Xcode 26)
TETHERKITNEXT_LIB_DIR=$PWD/build/lib swift build --package-path gui
TETHERKITNEXT_LIB_DIR=$PWD/build/lib swift test  --package-path gui
./gui/Scripts/build-gui.sh

# Full release build: universal libusb + universal C++ + tests + App + DMG
./scripts/build-release.sh

# Smoke-test CLI localization (English & Chinese)
./build/bin/tetherkitnext-cli --lang en --help
./build/bin/tetherkitnext-cli --lang zh --list
ctest --test-dir build -R common.i18n --output-on-failure
```
