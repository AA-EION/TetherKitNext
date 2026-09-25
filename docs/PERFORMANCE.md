# 性能调优指南

实测数字见 [BENCHMARKS.md](BENCHMARKS.md)，架构理由见 [DESIGN.md](DESIGN.md)。
本文只讲**怎么调**。

---

## 一句话结论

> **优化目标是每帧的系统调用次数，不是 CPU 周期。**

每帧的 CPU 成本合计约 70 ns，一次系统调用约 700 ns。macOS 上系统调用比 Linux
贵 3~10 倍，这是整个设计的第一约束。所以调优的全部着力点都在
「让一次系统调用 / 一次 USB 往返承载更多帧」。

---

## 吞吐的理论上限

| 链路 | 理论有效载荷 | 1514 字节满帧的帧率 |
|---|---|---|
| USB 2.0 full-speed | ~12 Mbps | ~1 k pps |
| USB 2.0 high-speed | **~426 Mbps**（53.2 MB/s） | ~35 k pps |
| USB 3.0 SuperSpeed | ~4 Gbps（RNDIS 实际很难跑满） | ~330 k pps |

高速的 426 Mbps 推导：`wMaxPacketSize=512`，每 125 µs 微帧最多 13 个数据包
→ 6656 B / 125 µs = 53.248 MB/s。

对照 `FrameRing` 跨线程搬运的 ~55 ns/帧（≈18 M 帧/秒），**无锁队列有两个数量级
的余量**。瓶颈永远在 USB 与系统调用侧。

---

## 调优旋钮（按收益排序）

### 1. `--max-transfer-kb`（默认 16）——**收益最大**

主机在 `INITIALIZE_MSG` 里宣称的 `MaxTransferSize`。这是让**设备**把多个
`REMOTE_NDIS_PACKET_MSG` 聚合进一次 bulk IN 的**唯一**手段。

- 报得越大 → 设备聚合越多 → 每帧摊到的 USB 往返与系统调用越少。
- Linux 只报 2048（一个满帧），非常保守。本项目默认 16 KiB。
- 上限：规范要求 USB 1.1 设备不超过 `0x4000`（16 KiB）。
- **代价**：bulk IN 缓冲会自动跟着放大（必须 ≥ 宣称值），内存占用
  = `rx-transfers × max-transfer-kb`。默认 16 × 16 KiB = 256 KiB。

⚠️ 但**设备可能不配合**：主线 Linux gadget 汇报 `MaxPacketsPerMessage = 1`，
即它一次只发一个包，报多大都没用。打了高通上行聚合补丁的 Android 内核才会报 3
或更多。启动日志里会打印协商结果，看 `设备聚合上限 N 包` 就知道有没有效。

### 2. `--rx-transfers`（默认 16）与 `--rx-transfer-kb`（默认 16）

并发在飞的 bulk IN 传输数与每个的大小。目标是**让 USB 控制器队列永不见底**。

- 在飞总量 = 两者相乘，默认 256 KiB。
- 依据：高速下 53 MB/s，1 ms 的队列空档就浪费 50 KB。在飞总量 ≥ 256 KiB 才能
  覆盖「用户态回调派发 + 重新 submit」的调度抖动。
- **不要用「多而小」**：macOS 上每次 `libusb_submit_transfer` 至少 3 次 IOKit
  往返（darwin 后端在每次提交前都调 `darwin_get_pipe_properties`，
  在 `IOUSBInterfaceInterface >= 550` 上等于两次 user-client 调用）。
  反过来 darwin 对单次 bulk 长度**没有**上限、不做分片
  （16 KB 分片是 Linux 后端的事，不是 macOS 的）。

### 3. `--bpf-buffer-kb`（默认 4096）

BPF 内核抓包缓冲。内核会分配**两份**（store + free 双缓冲），所以实际 wired
内存是这个值的两倍。

- 上限有歧义：调研的两个来源分别指向 `debug.bpf_maxbufsize`（512 KiB）与
  `debug.bpf_bufsize_cap`（32 MiB）。**代码不依赖哪个对** —— `BIOCSBLEN` 超限时
  内核**不报错**而是静默截断并把实际值写回参数，我们用写回值，并在被截断时打印
  一条 INFO 说明。
- 调大的收益有限：BPF 只有两份缓冲，增大单份并不能改善「copyout 期间 store 也满
  了」的丢包窗口。真丢包了应该先看 `--stats` 输出里的「内核丢包」，
  再考虑是不是 RX 注入线程跟不上。

### 4. `--mtu`（默认 1500）

- 上限受 `sysctl net.link.fake.max_mtu` 约束（本机 2048）。
- ⚠️ 该 sysctl 是 feth 的**创建期快照**，改了它对已存在的接口无效。
- ⚠️ BPF 写入还有一层硬限制：`hdrcmplt=1` 时整帧长度必须 ≤ 接口 MTU + 18
  （`BPF_WRITE_LEEWAY`）。
- 设备汇报的 `MaxTransferSize` 或 `OID_GEN_MAXIMUM_FRAME_SIZE` 更小时会被自动下调。

### 5. 编译选项

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
```

- **不要开 `-O3`**：热路径以 memcpy 与系统调用为主，`-O3` 的激进循环展开与向量化
  几乎无收益，反而增大代码体积、恶化 I-cache 命中。
- **`-DTETHERKITNEXT_NATIVE_ARCH=ON` 收益很小**：arm64 baseline 已经生成 LSE 原子
  指令（`ldadd`），`-mcpu=` 带来的只是调度模型差异，不是指令集解锁。
  且注意 `-mcpu=apple-m5` **会被 Apple clang 21 拒绝**（`native` 解析为 `apple-m4`）。
- **性能基准必须用未开消毒器的 Release 构建**，否则数字无参考价值。

---

## 怎么判断瓶颈在哪

`--stats 1000` 会每秒打印一行：

```
RX     3421 pps /   41.43 Mbps（丢 0）  |  TX     1180 pps /   14.29 Mbps（丢 0）  |  队列深度 0  内核丢包 0  背压 0
```

| 症状 | 含义 | 对策 |
|---|---|---|
| **队列深度**持续 > 0 且接近容量 | RX 注入线程（BPF write）跟不上 libusb 回调 | 确认 `批量写 已启用`（macOS 14+）；调大 `bridge.rx_write_batch` |
| **RX 丢** 增长 | RX 队列满，同上 | 同上；或调大 `bridge.rx_ring_frames` |
| **内核丢包** 增长 | BPF 内核缓冲溢出，TX 抽取线程读得不够快 | 调大 `--bpf-buffer-kb`；检查 tx-extract 线程是否被别的负载挤占 |
| **背压** 增长 | TX 传输池占满，USB 侧写不出去 | 调大 `data_channel.tx_transfer_count`；或者设备本身就是瓶颈 |
| **TX 丢** 增长但背压为 0 | 帧超长或链路 down 期间被丢 | 看 MTU 协商结果与链路状态日志 |
| RX/TX 都远低于预期且无丢包 | 设备侧或对端就是瓶颈 | 看启动日志里的链路速率与设备聚合上限 |

启动日志里这两行是判断「聚合有没有生效」的关键：

```
数据通道就绪：RX 16 × 16 KiB（在飞 256 KiB），TX 4 × 16 KiB；设备聚合上限 1580 字节 / 1 包，对齐 1 字节
数据路径已启动：RX 队列 2048 帧（4352 KiB），RX 写批 64 帧，TX 提交批 256 帧，链路批量写 可用
```

- `设备聚合上限 … / 1 包` → 设备不支持聚合，`--max-transfer-kb` 调大也没用。
- `链路批量写 不可用（逐帧写）` → 系统是 macOS 13 或更早，`BIOCSBATCHWRITE`
  不存在，RX 方向每帧一次系统调用，吞吐会明显低于 macOS 14+。

---

## 已知的性能限制

| 限制 | 影响 | 能否绕过 |
|---|---|---|
| macOS BPF **没有零拷贝**（无 `BIOCSETZBUF`） | 每方向一次内核↔用户拷贝 | 否。FreeBSD 有，Darwin 没有 |
| macOS **没有** CPU 亲和性 API | 数据路径线程可能被调度到效率核 | 否，只能用 QoS 表达意图（`thread_affinity_policy` 在 Apple Silicon 上基本无效） |
| `libusb_dev_mem_alloc` 在 darwin 返回 `NULL` | 没有零拷贝 DMA 缓冲 | 否。已改用 `posix_memalign` 按 `hw.pagesize`(16384) 对齐，减少 IOKit 建 DMA 描述符的分段 |
| Apple Silicon 上 libusb **控制传输**慢约 10 倍 | 保活与 OID 查询是毫秒级 | 否。因此保活周期不要调小，控制操作绝不放进数据热路径 |
| `BIOCSBATCHWRITE` 仅 macOS 14+ | macOS 13 及更早 RX 每帧一次系统调用 | 否，已自动回落 |
| feth 数据路径在 **> 5~8 Gbps** 时有内核 mbuf 溢出 panic 的社区报告 | 对本项目风险很低 | RNDIS over USB 2.0 实测 RX 325 / TX 235 Mbps，USB 3 也难超 1~2 Gbps，远低于该阈值 |
| ~~TX 高负载下背压丢帧~~ | ~~实测 6 秒内丢 1889 帧~~ | **已修复**：桥接层改为等待空闲传输槽位而不是丢弃，TX 丢帧降到 0。见 [BENCHMARKS.md](BENCHMARKS.md)「TX 丢帧的根因与修复」 |
| 双向并发时 RTT 会从 0.4 ms 膨胀到 16.6 ms | 修 TX 丢帧后已不再造成吞吐塌陷（双向 TX 4.8 → 87 Mbps），但延迟敏感场景仍受影响 | 部分能。缩小 RX 排队深度可缓解，但会牺牲 RX 峰值吞吐；改善幅度未实测 |
| TCP TX 呈双峰（约 240 / 约 300 Mbps 两个稳定态） | 吞吐不稳定，但两态都是零重传，不是丢包 | 原因未查明 |

---

## 实测结论（2026-07-28 补齐）

本节此前列的四项「未经实测」现在**全部有实测值了**。完整数据与方法见
[BENCHMARKS.md](BENCHMARKS.md)，这里只记结论：

| 原先的估值 | 实测值 | 差异 |
|---|---|---|
| BPF `write()` ~700 ns（来自调研的 `write(/dev/null)`） | **2181 ns** | **贵约 3 倍** |
| `BIOCSBATCHWRITE` 提速倍数未知 | 805 ns/帧（batch ≥ 32） | **约 2.7 倍** |
| 端到端吞吐能否接近 426 Mbps | RX **324~327 Mbps**（理论上限的 77%） | 接近但达不到 |
| feth 往返延迟未知 | **24.5 µs** | 只占端到端 RTT 的约 5% |

三条要点：

1. **BPF 写入不是瓶颈。** 虽然实测比估值贵 3 倍，但批量写下仍有 124 万帧/秒
   （约 15 Gbps @1514 字节），而 USB 2.0 满速只需约 2.7 万帧/秒 —— 余量 45 倍。
   本文档早先「BPF 写入决定 RX 方向理论上限」的担心可以放下。
2. **这笔开销几乎与帧长无关**（64 字节 1934 ns vs 1514 字节 2181 ns，只差 13%），
   全是每次调用的固定成本 —— 所以批量写才是关键，而不是缩小帧。
3. **TX 方向曾是弱侧，根因已查明并修复。** 现象是丢帧发生在本项目自己的 TX 路径
   上；根因是桥接层在 bulk OUT 传输池占满时**立即丢弃**批次剩余帧，而槽位其实
   只需等几百微秒就会释放。改为等待后：TX 丢帧 1889 → **0**，TCP TX 重传
   3829 → **0**，双向并发 TX **4.8 → 87 Mbps**。
   顺带否掉了原先的调优建议 —— 修复后 `--tx-transfers` 取 4 / 8 / 16 已无可分辨
   差异，默认值保持 4。详见 BENCHMARKS.md「TX 丢帧的根因与修复」。

**教训**：那段丢弃逻辑当时附了一条很有说服力的注释（「等待会让内核缓冲堆积成
看不见的丢包」），而两个前提都是错的 —— `bs_drop` 每批都读回来并显示在统计行里，
且 4 MiB 内核缓冲正是该吸收突发的那一级。**写下「刻意这样做」的理由时，
要能说出它的量级**：这里池深 4 个槽 vs 一个分片需要约 26 个槽，量级一对就露馅了。

仍然没查清的：TCP TX 的双峰行为、以及双向并发下的 RTT 膨胀。
清单见 [BENCHMARKS.md](BENCHMARKS.md) 末节。
