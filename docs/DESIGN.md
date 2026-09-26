# TetherKitNext Design Document

This document explains **why TetherKitNext is designed the way it is**. For exact wire field offsets and constants, see [RNDIS-PROTOCOL.md](RNDIS-PROTOCOL.md); for measured numbers, see [BENCHMARKS.md](BENCHMARKS.md); for verified environment facts and lessons learned, see [../AGENTS.md](../AGENTS.md).

---

## 1. Problem and Approach Selection

The macOS kernel has **no** built-in RNDIS driver. This was verified by inspecting the `IOKitPersonalities` of every CDC-family kext under `/System/Library/Extensions`:

| kext personality | Matching Condition |
|---|---|
| `AppleUSBACMControl0` | class 2, subclass 2, protocol **0** |
| `AppleUSBACMControl1` | class 2, subclass 2, protocol **1** |
| `AppleUSBECMControl` | class 2, subclass **6** |
| `AppleUSBNCMControl` | class 2, subclass **13** |
| `AppleUSBWCMControl` | class 2, subclass **8** |

An RNDIS communication interface uses `{0x02, 0x02, 0xFF}` — its protocol is neither 0 nor 1, so **no personality matches**. The common Android variant `{0xE0, 0x01, 0x03}` does not even have a personality for class `0xE0`.

To use an RNDIS device on macOS, there are only four possible approaches:

| Approach | Pure User Space? | Requirements | Semantic Layer | Verdict |
|---|---|---|---|---|
| kext / NKE | ❌ | Apple-granted kext signing entitlement or disabling SIP | L2 | Practically unviable on Apple Silicon |
| `NEPacketTunnelProvider` | ✔ | Paid developer account + entitlement + App bundle | **L3** | Requires re-implementing ARP/ND/DHCP |
| `utun` | ✔ | root | **L3** | Same as above, plus manual Ethernet header stripping/framing |
| **`feth` + BPF** | ✔ | root | **L2** | ✅ Selected by TetherKitNext |

The decisive reason for choosing `feth` + BPF is **semantic layer alignment**: RNDIS is natively Layer 2 (carrying full Ethernet frames), and `feth` + BPF is the only pure user-space mechanism on macOS capable of sending and receiving **raw Ethernet frames** bidirectionally. Because of this, the phone's DHCP server and ARP broadcasts work directly with the macOS IP stack, sparing us from implementing ARP proxy, IPv6 Neighbor Discovery, or a custom DHCP client — which are the real sources of complexity in Layer 3 approaches.

Trade-offs: requires root privileges (both for creating `feth` interfaces and opening `/dev/bpf*`), adds one extra mbuf copy per direction, and imposes a TX length cap of `total frame ≤ MTU + 18`.

---

## 2. High-Level Architecture

```
        ┌──────────────────────── macOS Kernel ────────────────────────┐
        │  IP Stack / Routing / DHCP Client                            │
        │       │                                                      │
        │       ▼                                                      │
        │  ┌─────────┐    if_fake peer pair    ┌─────────┐             │
        │  │  feth0  │ ◄─────────────────────► │  feth1  │             │
        │  │(host)   │                         │(driver) │             │
        │  └─────────┘                         └────┬────┘             │
        │   IP / Routes configured                  │ BPF (single fd   │
        │   MAC = device-reported address           │  for RX and TX)  │
        └───────────────────────────────────────────┼──────────────────┘
                                                    │
        ┌───────────────────────────────────────────┼──────────────────┐
        │  TetherKitNext (User Space)               ▼                  │
        │  ┌────────────── core::Bridge (3 data-path threads) ──────┐  │
        │  │  RX: bulk IN callback → FrameRing → BPF batch write    │  │
        │  │  TX: BPF batch read → RNDIS multi-packet → bulk OUT    │  │
        │  └──────────────────────┬─────────────────────────────────┘  │
        │  ┌───────── rndis::StateMachine (control thread) ───┐        │
        │  │  INITIALIZE / QUERY / SET / KEEPALIVE / RESET /  │        │
        │  │  HALT / INDICATE_STATUS                          │        │
        │  └──────────────────────┬───────────────────────────┘        │
        │  ┌───────── usb::Device / DataChannel ──────────────┐        │
        │  │  Control channel (EP0 class reqs) + Interrupt IN │        │
        │  │  Async bulk IN/OUT transfer pools                │        │
        │  └──────────────────────┬───────────────────────────┘        │
        └─────────────────────────┼────────────────────────────────────┘
                                  │ USB
                             ┌────┴───────┐
                             │RNDIS Device│
                             └────────────┘
```

### Modules and Dependency Direction

Dependencies are **strictly unidirectional**; reverse or circular dependencies are forbidden:

```
tk_common  ← (no deps)        Errors, logging, byte order, lock-free rings, stats, thread QoS
   ↑
tk_rndis   ← common           RNDIS wire format + state machine (**pure logic, no I/O**)
tk_net     ← common           feth lifecycle + BPF link
tk_usb     ← common, rndis    libusb wrapper (rndis used only for protocol constants)
   ↑
tk_core    ← all of above     Data-path bridge + runtime orchestration
   ↑
tetherkitnext ← core          CLI executable
```

**Keeping `tk_rndis` free of I/O is a deliberate architectural constraint**: it allows the entire RNDIS implementation (including the state machine) to be thoroughly unit-tested on a machine with neither a USB device nor root privileges. For this reason, the `ControlChannel` interface lives in the `rndis` layer (describing RNDIS protocol semantics rather than USB specifics) and is implemented by `tk_usb`.

---

## 3. Data-Flow Semantics (Crucial and Easy to Get Wrong)

Verified directly against XNU's `feth_output_common()` source code:

| Event | On `feth0` | On `feth1` |
|---|---|---|
| Host IP stack transmits a frame out of `feth0` | **OUT** tap | **IN** tap, delivered as input |
| Driver writes a frame to `feth1`'s BPF fd | Delivered as **input** to host IP stack | **OUT** tap |

Two key conclusions follow from this:

1. **BPF only needs to attach to `feth1` (the driver side); a single file descriptor handles both RX and TX.**
2. **`BIOCSSEESENT = 0` must be set** (→ `bd_direction = BPF_D_IN`), which filters out frames that **we wrote into `feth1` ourselves** (since those appear in the OUT direction on `feth1`). Without this setting, every injected frame would immediately loop back and be read again.

---

## 4. Concurrency Model

There are three data-path threads, plus libusb's internal IOKit runloop thread and the RNDIS state-machine control thread. On a 4-performance-core + 6-efficiency-core Apple Silicon machine, the three hot threads plus the libusb runloop fit neatly onto the performance core cluster without oversubscription.

| Thread | Responsibility | Blocking Point | QoS |
|---|---|---|---|
| `usb-event` | `libusb_handle_events`, running all transfer callbacks | event pipe | USER_INTERACTIVE |
| `rx-inject` | Batch-dequeue from `FrameRing` → BPF batch `write` | Parks on futex doorbell when ring is empty | USER_INTERACTIVE |
| `tx-extract` | Blocking BPF `read` → RNDIS aggregation → async bulk OUT | BPF `read()` | USER_INTERACTIVE |
| `rndis-ctl` | State machine `Poll()`: keepalive + draining status notifications | `sleep` | USER_INITIATED |

### Why RX Must Be Split Across Two Threads

libusb's transfer callbacks hold `ctx->event_waiters_lock`. Performing blocking I/O inside a callback would stall **every thread waiting on synchronous transfers** (including the state-machine control thread). Since BPF `write()` is a system call (~700–2100 ns), the USB callback only unpacks RNDIS packets, `memcpy`s frames into the lock-free `FrameRing`, and immediately resubmits the transfer. Actual BPF writing is offloaded to `rx-inject`.

### Why TX Needs Only One Thread

BPF `read()` is natively blocking and self-batching (with `BIOCIMMEDIATE=1`, it wakes as soon as the first packet arrives and delivers all packets accumulated in the meantime in one syscall), while `SendFrames` submits bulk OUT transfers **asynchronously** without blocking. Reading a batch and submitting a batch on the same thread is optimal; adding another thread would only introduce an extra cross-core handoff.

### Why Not Merge Everything into a Single `kqueue` Event Loop

Benchmarks on macOS show that an empty non-blocking `kevent()` (`timeout={0,0}`, no events) takes **13.4–13.7 µs** — ~20× more expensive than an ordinary syscall. At 25 kpps, polling alone would consume 34% of a core. **`kqueue` should only be used for blocking waits, never for polling** — and since each of our three paths already has its own natural blocking primitive, merging them brings no benefit.

### Why the Control Channel Must Run on a Dedicated Non-Event Thread

libusb's synchronous APIs (`libusb_control_transfer` / `libusb_interrupt_transfer`) begin with `if (usbi_handling_events(ctx)) return LIBUSB_ERROR_BUSY;` — a thread-local check that guarantees failure if called from the event thread (including inside any transfer callback). Thus the state machine cannot run inside the USB event loop and requires its own thread.

### Why `THREAD_TIME_CONSTRAINT_POLICY` Is Not Used

Time-constraint scheduling guarantees `computation` CPU time per `period`, which suits strictly periodic real-time audio threads. Our threads spend most of their time blocked in `read()`, `write()`, or `handle_events()`. Enabling time-constraint policy would (a) strip the thread's QoS class and (b) cause the kernel to demote the thread whenever a burst exceeds `computation`. Furthermore, macOS has **no** CPU affinity API (`thread_affinity_policy` is a no-op on Apple Silicon), so priority/QoS classes are the right mechanism.

---

## 5. Performance Design

See [BENCHMARKS.md](BENCHMARKS.md) for full measurements. Core principle:

> **The optimization target is system calls per frame, not CPU cycles.**

Per-frame CPU cost totals ~70 ns (RNDIS codec ~15 ns + ring transfer ~55 ns + stats ~0.5 ns), whereas a syscall costs ~700–2100 ns — **an order of magnitude more than CPU processing**.

This leads to four primary optimizations:

| Optimization | Mechanism | Effect |
|---|---|---|
| RX batch write | `BIOCSBATCHWRITE` (macOS 14+, runtime feature detection, per-frame fallback) | One syscall per batch of frames |
| TX multi-packet aggregation | RNDIS allows multiple `PACKET_MSG` records in a single bulk OUT transfer | One USB round-trip per batch of frames |
| Device RX aggregation | Advertise a large `MaxTransferSize` in `INITIALIZE_MSG` (default 16 KiB) | **Primary lever for RX throughput** |
| Batch-published lock-free ring | `FrameRing::BatchWrite/BatchRead` performs one release store per batch | 2.4× speedup for 1514B frames, 5.0× for 64B frames |

### Why Single-Copy Instead of Zero-Copy

The RX path performs one `memcpy` per frame (USB transfer buffer → `FrameRing` slot). True zero-copy would require handing ownership of USB transfer buffers downstream, preventing immediate resubmission and requiring a much larger buffer pool plus a return channel — whereas `memcpy` of 1514 bytes takes only 25–70 ns (a fraction of a syscall). **Pursuing zero-copy here would optimize the wrong thing.**

On the TX path, zero-copy is **architecturally impossible**: `struct bpf_hdr` is only 20 bytes, whereas an RNDIS packet header requires 44 bytes (leaving a 24-byte headroom deficit), and `feth`'s `tx_headroom` is 32 bytes (still insufficient).

### 128-Byte Cache Lines

`sysctl hw.cachelinesize` on Apple Silicon reports **128** (not the traditional 64). Aligning to 64 bytes would leave producer and consumer indices on the same 128-byte cache line, causing false sharing.

We deliberately avoid `std::hardware_destructive_interference_size`: while Apple libc++ defines it in `<new>`, it reports **256**, which would needlessly double all padding.

---

## 6. Error Handling

| Path | Mechanism | Rationale |
|---|---|---|
| Initialization / Control | `std::expected<T, Error>` | Most failures are expected environmental conditions (no device plugged in, non-root, interface busy) that callers must handle explicitly without exceptions |
| **Data Hot Path** | **Return counts + atomic counters** | `std::string` inside `Error` allocates heap memory, which is unacceptable at 25k–80k pps |

`Error` carries an error domain (`errno`, `libusb`, `RNDIS_STATUS`, or logic), and `ToString()` formats domain codes (e.g., translating libusb `-3` into `LIBUSB_ERROR_ACCESS(-3)`). `WithContext` chains outer causes using a localized separator (`detail::ContextSeparator()`).

All user-visible error messages come from the localization table (Section 6b), never raw string literals.

**Mapping RNDIS status codes to symbolic names lives exclusively in `rndis/protocol.cc`** (single source of truth); `tk_common` has no knowledge of RNDIS and formats raw hex values, while the `rndis` layer attaches symbolic names to the context string.

---

## 6b. Localization (Chinese / English)

User-facing text (errors, logs, CLI help, state names) **never uses hardcoded string literals**; all strings live in `include/tetherkitnext/common/messages.def`. This X-macro table expands three times to generate the `Msg` enum, the Chinese string table, and the English string table from a single source — making it **structurally impossible for a message to be missing in one language**.

| Accessor | Purpose | Allocates? |
|---|---|---|
| `Tr(Msg::kFoo, args...)` | Parameterized formatting (`std::format` semantics) | Yes (returns `std::string`) |
| `Text(Msg::kFoo)` | Unparameterized lookup (`string_view`) | No, `noexcept` |
| `TETHERKITNEXT_INFO_TR(Msg::kFoo, ...)` etc. | Logging macros | Arguments are not evaluated if the log level is disabled |

**Why not `gettext`**: `gettext` requires linking `libintl`, running `msgfmt` at build time, and looking up `.mo` files on disk at runtime — introducing runtime failure modes when `.mo` files cannot be found. For two languages and a few hundred static strings, compiling the table directly into the binary is simpler and self-contained.

**Trade-off and safeguard**: because format strings are looked up at runtime, `std::format`'s compile-time placeholder check is bypassed (`Tr` uses `vformat` internally). `tests/test_common_i18n.cc` restores this guarantee by iterating over every entry in `messages.def`, verifying that both languages reference identical argument indices and format specifiers, and checking that indices are contiguous without mixing automatic and manual indexing. Any placeholder mismatch fails immediately under `ctest -R common.i18n`.

Like `Error`, `Tr()` is **forbidden on the data hot path** because it allocates; hot paths may only use `Text()`.

Language selection is controlled by the host process: the CLI inspects `--lang` and environment variables, while the GUI pushes language changes via C ABI `tk_set_language`. Language state is a **process-wide atomic** so all threads emit logs in a consistent language.

---

## 7. Backpressure Strategy

| Location | Behavior When Full | Rationale |
|---|---|---|
| RX Ring (USB → BPF) | **Drop and increment counter** | libusb callbacks cannot block (they hold internal locks) |
| TX Transfer Pool (BPF → USB) | **Wait briefly for an in-flight transfer to complete; drop and count only on timeout/shutdown** | Waiting absorbs micro-bursts in the 4 MiB BPF buffer without premature frame loss (see [BENCHMARKS.md](BENCHMARKS.md)) |
| RX During Pause | **Retain** queued frames and deliver after resume | The link is only transiently paused; queued frames remain valid |
| TX During Pause | **Drop and increment counter** | During an RNDIS soft reset, the device discards all pending packets anyway |

---

## 8. Teardown Order (Preventing Use-After-Free)

`libusb_close` **does not** reap in-flight transfers — it merely removes the device from the list, nulls out `dev_handle`, and logs a warning **without invoking callbacks or freeing transfer memory**. Subsequent IOKit aborts can still trigger `darwin_async_io_callback` on libusb's internal thread; freeing transfers before those callbacks finish causes a use-after-free crash.

The required teardown order (implemented jointly by `Runtime::Stop()` and `UsbDataChannel::Shutdown()`):

```
1. Bridge::Stop()
     ├ Set shutdown flag
     ├ link->Interrupt()        Break blocking BPF read
     ├ Join rx-inject / tx-extract threads
     └ DataChannel::Shutdown()
          ├ Set shutdown flag (callbacks stop resubmitting)
          ├ Cancel all in-flight transfers
          ├ **Wait for in-flight count to reach zero** (every callback has returned)
          └ Only then call libusb_free_transfer + free buffers
2. Close BPF fd
3. Destroy feth interface pair
4. StateMachine::Stop(): SET filter=0 → HALT
5. Release USB interfaces and close device handle
6. Stop libusb event thread and call libusb_exit
```

Two critical invariants:

- **Waiting for the in-flight counter to reach zero in Step 1 must never happen on the libusb event thread** — the callbacks that decrement the counter run on that thread, so waiting there would deadlock.
- If waiting times out, **deliberately leak** the transfer buffers rather than freeing them while IOKit may still access them, and log an explicit error.

---

## 9. Testability

Because development and CI machines may have **neither a physical RNDIS device nor root privileges**, every external resource boundary is abstracted behind an interface:

| Interface | Production Implementation | Test Implementation |
|---|---|---|
| `rndis::ControlChannel` | `usb::UsbControlChannel` | `testing::MockControlChannel` |
| `usb::DataChannel` | `usb::UsbDataChannel` | `testing::MockDataChannel` |
| `net::LinkBackend` | `net::BpfLink` | `net::LoopbackLink` |

**Abstraction granularity is per-batch rather than per-frame**: one virtual call processes dozens or hundreds of frames, amortizing virtual dispatch overhead to well under 1 ns per frame.

This allows the entire state machine (including out-of-order device indications, keepalive timeouts, and soft-reset replay) and the entire data bridge (bidirectional load, backpressure, pause/resume, and shutdown under load) to be tested offline and verified under ThreadSanitizer. Root-only `feth`/BPF tests are **skipped rather than failed** by default and enabled via `TETHERKITNEXT_ROOT_TESTS=1`.

---

## 10. Private ABI Usage and Risk Mitigation

TetherKitNext uses three interfaces not present in the public macOS SDK headers:

| Private ABI | Purpose | Risk & Mitigation |
|---|---|---|
| `struct ifdrv` | Parameter to `SIOCSDRVSPEC` | Structure size is encoded into the ioctl number; a size mismatch produces a non-existent ioctl (`ENOTTY`, safe failure). `static_assert` locks down `sizeof == 40`, field offsets, and `SIOCSDRVSPEC == 0x8028697b` |
| `struct if_fake_request` | `feth` peer pairing | Identical across every XNU release tag from xnu-7195 (macOS 11) through xnu-12377 (macOS 26), and depended upon by Apple's own `/sbin/ifconfig fethN peer fethM`. Locked down with `static_assert(sizeof == 160)` |
| `BIOCSBATCHWRITE` / `BIOCSNOTSTAMP` | Performance optimizations | **Strictly optional**. Probed at runtime via ioctl; if unsupported, falls back to standard per-frame writes without affecting correctness |

In short: **functional ABIs have remained stable for 15+ years and are guarded by `static_assert` and CI ABI gates; optimization ABIs use runtime feature detection with safe fallbacks.**

---

## 11. Coding Conventions

| Category | Convention | Example |
|---|---|---|
| Namespace | `lower_case` | `tetherkitnext::rndis` |
| Types | `CamelCase` | `PacketMessageWriter` |
| Functions & Methods (including accessors) | `CamelCase` | `MaxFrameBytes()` |
| Local variables & parameters | `lower_case` | `frame_length` |
| Private member variables | `lower_case_` | `bulk_in_endpoint_` |
| Constants / Enumerator values | `k` + `CamelCase` | `kPacketMsgHeaderBytes` |
| Files | `snake_case`, `.h` / `.cc` | `packet_codec.h` |
| Message IDs | `k` + module prefix + `CamelCase` | `kNetBpfBindFailed`, `kCliUnknownOption` |

Accessors also use `CamelCase` (rather than STL-style `lower_case`) so that `readability-identifier-naming` can enforce naming mechanically without manual exceptions.

`.clang-tidy` disables specific checks unsuitable for low-level systems code, with documented rationale for each. In particular, `performance-enum-size` is disabled because protocol enum underlying types **must** match exact wire field widths (LE32 in RNDIS).
