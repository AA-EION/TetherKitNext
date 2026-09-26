# Performance Tuning Guide

See [BENCHMARKS.md](BENCHMARKS.md) for measured numbers and [DESIGN.md](DESIGN.md) for architectural rationale.
This document focuses strictly on **how to tune**.

---

## One-Sentence Takeaway

> **The optimization target is system calls per frame, not CPU cycles.**

Per-frame CPU cost totals roughly 70 ns, whereas a single system call costs roughly 700 ns. System calls on macOS are 3–10× more expensive than on Linux, which is the primary constraint across the entire design. Consequently, every tuning effort centers on **carrying more frames per system call and per USB round-trip**.

---

## Theoretical Throughput Limits

| Link | Theoretical Payload | Frame Rate at 1514-Byte Full Frames |
|---|---|---|
| USB 2.0 full-speed | ~12 Mbps | ~1 k pps |
| USB 2.0 high-speed | **~426 Mbps** (53.2 MB/s) | ~35 k pps |
| USB 3.0 SuperSpeed | ~4 Gbps (RNDIS rarely saturates this in practice) | ~330 k pps |

Derivation of the 426 Mbps high-speed limit: `wMaxPacketSize=512`, up to 13 data packets per 125 µs microframe → 6656 B / 125 µs = 53.248 MB/s.

Compared against `FrameRing` cross-thread transfer at ~55 ns/frame (≈18 M frames/sec), **the lock-free ring has two orders of magnitude of headroom**. The bottleneck is always on the USB and system-call sides.

---

## Tuning Knobs (Ordered by Impact)

### 1. `--max-transfer-kb` (Default: 16) — **Highest Impact**

The `MaxTransferSize` advertised by the host in `INITIALIZE_MSG`. This is the **only** mechanism that allows the **device** to aggregate multiple `REMOTE_NDIS_PACKET_MSG` records into a single bulk IN transfer.

- Larger advertised value → more device aggregation → fewer USB round-trips and system calls amortized per frame.
- Linux advertises only 2048 (a single full frame), which is very conservative. TetherKitNext defaults to 16 KiB.
- Upper limit: the specification requires no more than `0x4000` (16 KiB) for USB 1.1 devices.
- **Trade-off**: bulk IN buffers scale up automatically (must be ≥ the advertised value), so memory usage = `rx-transfers × max-transfer-kb`. Default: 16 × 16 KiB = 256 KiB.

⚠️ **The device might not cooperate**: the mainline Linux gadget driver reports `MaxPacketsPerMessage = 1`, meaning it sends only one packet per transfer regardless of how large a value we advertise. Android kernels with Qualcomm uplink aggregation patches report 3 or more. The startup log prints the negotiated result; check the device aggregation limit (`N packets`) to see whether aggregation is active.

### 2. `--rx-transfers` (Default: 16) and `--rx-transfer-kb` (Default: 16)

The number of concurrent in-flight bulk IN transfers and the size of each transfer. The goal is to **keep the USB host controller queue from ever running dry**.

- Total in-flight capacity = product of both values, 256 KiB by default.
- Rationale: at 53 MB/s on USB 2.0 high-speed, a 1 ms queue gap wastes ~50 KB. Keeping ≥ 256 KiB in flight absorbs scheduling jitter between user-space callback dispatch and transfer resubmission.
- **Avoid "many small transfers"**: on macOS, each `libusb_submit_transfer` incurs at least 3 IOKit round-trips (the Darwin backend calls `darwin_get_pipe_properties` before every submission, which translates to two user-client calls on `IOUSBInterfaceInterface >= 550`). Conversely, Darwin imposes **no** upper bound on a single bulk transfer size and performs no fragmentation (16 KB fragmentation is a Linux backend quirk, not macOS).

### 3. `--bpf-buffer-kb` (Default: 4096)

The kernel BPF capture buffer size. The kernel allocates **two buffers** (a store + free double buffer), so actual wired kernel memory is twice this setting.

- Upper-bound ambiguity: two different sources point to `debug.bpf_maxbufsize` (512 KiB) vs. `debug.bpf_bufsize_cap` (32 MiB). **The code does not rely on which one is authoritative** — when `BIOCSBLEN` exceeds the kernel cap, the kernel **does not fail**; instead, it silently clamps the value and writes the actual size back into the ioctl parameter. We use the written-back value and log an INFO message if clamping occurred.
- Increasing this yields diminishing returns: BPF only has two buffers, so enlarging a single buffer does not eliminate the drop window when the store buffer fills up during a `copyout`. If drops occur, check `kernel drops` in the `--stats` output first, and then check whether the RX injection thread is falling behind.

### 4. `--mtu` (Default: 1500)

- Capped by `sysctl net.link.fake.max_mtu` (2048 on macOS).
- ⚠️ That sysctl is snapshotted **at `feth` creation time**; changing it afterward has no effect on existing interfaces.
- ⚠️ BPF writes enforce an additional hard limit: when `hdrcmplt=1`, the total frame length must be ≤ interface MTU + 18 (`BPF_WRITE_LEEWAY`).
- If the device reports a smaller `MaxTransferSize` or `OID_GEN_MAXIMUM_FRAME_SIZE`, the MTU is lowered automatically.

### 5. Build Options

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
```

- **Do not enable `-O3`**: the hot path is dominated by `memcpy` and system calls. Aggressive loop unrolling and vectorization from `-O3` provide virtually no gain while bloating code size and hurting I-cache locality.
- **`-DTETHERKITNEXT_NATIVE_ARCH=ON` yields minimal benefit**: the arm64 baseline already emits LSE atomic instructions (`ldadd`), so `-mcpu=` only adjusts instruction scheduling rather than unlocking new instruction sets. Note also that `-mcpu=apple-m5` is **rejected by Apple clang 21** (`native` resolves to `apple-m4`).
- **Benchmarks must always run on Release builds without sanitizers**, otherwise the numbers are meaningless.

---

## Diagnosing Bottlenecks

`--stats 1000` prints one line per second:

```
RX     3421 pps /   41.43 Mbps (drop 0)  |  TX     1180 pps /   14.29 Mbps (drop 0)  |  ring depth 0  kernel drops 0  backpressure 0
```

| Symptom | Meaning | Remedy |
|---|---|---|
| **Ring depth** stays > 0 and near capacity | RX injection thread (BPF write) cannot keep up with libusb callbacks | Verify batch write is enabled (macOS 14+); increase `bridge.rx_write_batch` |
| **RX drop** increasing | RX ring is full (same root cause as above) | Same as above, or increase `bridge.rx_ring_frames` |
| **Kernel drops** increasing | BPF kernel buffer overflow; TX extraction thread is reading too slowly | Increase `--bpf-buffer-kb`; check if `tx-extract` is starved by other workloads |
| **Backpressure** increasing | TX transfer pool exhausted; USB side cannot drain fast enough | Increase `data_channel.tx_transfer_count`, or the device itself is the bottleneck |
| **TX drop** increasing while backpressure is 0 | Oversized frames or drops while the link is down | Check MTU negotiation results and link-status logs |
| Both RX/TX well below expectations with zero drops | Device or remote peer is the bottleneck | Check link speed and device aggregation limit in startup logs |

These two lines in the startup log indicate whether aggregation is effective:

```
Data channel ready: RX 16 × 16 KiB (256 KiB in flight), TX 4 × 16 KiB; device aggregation limit 1580 bytes / 1 packets, alignment 1 bytes
Data path started: RX ring 2048 frames (4352 KiB), RX write batch 64 frames, TX submit batch 256 frames, link batch write available
```

- `device aggregation limit … / 1 packets` → The device does not support aggregation; increasing `--max-transfer-kb` will not help.
- `link batch write unavailable (per-frame write)` → Running on macOS 13 or earlier where `BIOCSBATCHWRITE` does not exist; RX incurs one syscall per frame and throughput will be noticeably lower than on macOS 14+.

---

## Known Performance Limitations

| Limitation | Impact | Workaround Available? |
|---|---|---|
| macOS BPF **has no zero-copy** (no `BIOCSETZBUF`) | One kernel↔user copy per direction | No. FreeBSD has it; Darwin does not |
| macOS **has no CPU affinity API** | Data-path threads may be scheduled onto efficiency cores | No; we can only express intent via QoS (`thread_affinity_policy` is effectively a no-op on Apple Silicon) |
| `libusb_dev_mem_alloc` returns `NULL` on Darwin | No zero-copy DMA buffers | No. Uses `posix_memalign` aligned to `hw.pagesize` (16384) to reduce IOKit DMA descriptor scatter-gather segments |
| libusb **control transfers** are ~10× slower on Apple Silicon | Keepalive and OID queries take milliseconds | No. Keepalive intervals should not be shortened, and control operations must never enter the data hot path |
| `BIOCSBATCHWRITE` is macOS 14+ only | One syscall per RX frame on macOS 13 and earlier | No; automatically falls back to per-frame writes |
| Community reports of kernel mbuf exhaustion panic on `feth` at **> 5–8 Gbps** | Very low risk for this project | RNDIS over USB 2.0 measures ~325 Mbps RX / ~235 Mbps TX, and even USB 3 rarely exceeds 1–2 Gbps—well below that threshold |
| ~~TX backpressure frame drops under heavy load~~ | ~~Dropped 1889 frames in 6 seconds~~ | **Fixed**: the bridge now waits for a free transfer slot instead of dropping immediately, reducing TX drops to 0. See "Root Cause and Fix of TX Frame Drops" in [BENCHMARKS.md](BENCHMARKS.md) |
| RTT inflates from 0.4 ms to 16.6 ms under bidirectional load | No longer causes throughput collapse after fixing TX drops (bidirectional TX improved from 4.8 → 87 Mbps), but latency-sensitive traffic is still affected | Partially. Reducing RX queue depth can mitigate bufferbloat at the cost of peak RX throughput; exact trade-off not yet measured |
| Bimodal TCP TX throughput (~240 Mbps vs. ~300 Mbps stable states) | Throughput fluctuates between two stable modes, though both have zero retransmissions (not packet loss) | Root cause not yet identified |

---

## Measured Conclusions (Completed 2026-07-28)

All four items previously listed as "unmeasured estimates" now have real hardware measurements. Full data and methodology are in [BENCHMARKS.md](BENCHMARKS.md); below is the summary:

| Original Estimate | Measured Value | Difference |
|---|---|---|
| BPF `write()` ~700 ns (extrapolated from `write(/dev/null)`) | **2181 ns** | **~3× more expensive** |
| `BIOCSBATCHWRITE` speedup factor unknown | 805 ns/frame (batch ≥ 32) | **~2.7× speedup** |
| Whether end-to-end throughput can approach 426 Mbps | RX **324–327 Mbps** (77% of theoretical limit) | Close, though bounded by USB/gadget overhead |
| `feth` round-trip latency unknown | **24.5 µs** | Accounts for only ~5% of end-to-end RTT |

Three key takeaways:

1. **BPF writing is not the bottleneck.** Although single writes are 3× more expensive than estimated, batch writes still achieve 1.24 million frames/sec (~15 Gbps at 1514 bytes), whereas USB 2.0 high-speed requires only ~27k frames/sec — a 45× safety margin. Earlier concerns that BPF writes would cap theoretical RX throughput can be put to rest.
2. **This overhead is nearly independent of frame length** (1934 ns for 64 bytes vs. 2181 ns for 1514 bytes, a 13% difference). Almost all of the cost is per-call fixed overhead, which is why batch writing is critical rather than shrinking frames.
3. **The TX path was previously the weak side; its root cause has been identified and fixed.** Drops were occurring in our own TX path because the bridge **immediately discarded** remaining frames in a batch whenever the bulk OUT transfer pool was full, even though a slot would become free within a few hundred microseconds. Switching to bounded waiting reduced TX drops from 1889 → **0**, TCP TX retransmissions from 3829 → **0**, and boosted bidirectional concurrent TX from **4.8 → 87 Mbps**. This also invalidated an earlier tuning recommendation: after the fix, setting `--tx-transfers` to 4, 8, or 16 shows no discernible difference, so the default remains 4. See [BENCHMARKS.md](BENCHMARKS.md) for details.

**Lesson learned**: The original drop logic had a convincing comment claiming that waiting would cause invisible kernel buffer drops. Both premises were false: `bs_drop` is read back on every batch and displayed in the stats line, and the 4 MiB kernel buffer is specifically meant to absorb bursts. **When writing a justification for a deliberate design choice, always check the orders of magnitude**: a pool depth of 4 slots vs. ~26 slots needed for a burst immediately reveals the mismatch.

Still open: the bimodal TCP TX behavior and RTT inflation under bidirectional load. See the final section of [BENCHMARKS.md](BENCHMARKS.md).
