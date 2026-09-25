# TetherKitNext 设计文档

本文说明**为什么这样设计**。具体的字段偏移与常量见
[RNDIS-PROTOCOL.md](RNDIS-PROTOCOL.md)；实测数字见 [BENCHMARKS.md](BENCHMARKS.md)；
已验证的环境事实与踩过的坑见 [../AGENTS.md](../AGENTS.md)。

---

## 1. 问题与方案选择

macOS 内核**没有** RNDIS 驱动。这不是推测 —— 逐个检查了
`/System/Library/Extensions` 下所有 CDC 家族 kext 的 `IOKitPersonalities`：

| kext personality | 匹配条件 |
|---|---|
| `AppleUSBACMControl0` | class 2, subclass 2, protocol **0** |
| `AppleUSBACMControl1` | class 2, subclass 2, protocol **1** |
| `AppleUSBECMControl` | class 2, subclass **6** |
| `AppleUSBNCMControl` | class 2, subclass **13** |
| `AppleUSBWCMControl` | class 2, subclass **8** |

RNDIS 的通信类接口是 `{0x02, 0x02, 0xFF}` —— protocol 既不是 0 也不是 1，
**没有任何 personality 匹配**；Android 常用的 `{0xE0, 0x01, 0x03}` 变体连
class `0xE0` 的 personality 都不存在。

于是要在 macOS 上用起 RNDIS 设备，只有四条路：

| 方案 | 能否纯用户态 | 门槛 | 语义层级 | 结论 |
|---|---|---|---|---|
| kext / NKE | ❌ | Apple 授予的 kext 签名 entitlement 或关 SIP | L2 | Apple Silicon 上实质不可行 |
| `NEPacketTunnelProvider` | ✔ | 付费开发者账号 + entitlement + App bundle | **L3** | 要自己实现 ARP/ND/DHCP |
| `utun` | ✔ | root | **L3** | 同上，且要自己拆装以太头 |
| **`feth` + BPF** | ✔ | root | **L2** | ✅ 本项目选择 |

选 `feth` + BPF 的决定性理由是**语义层级匹配**：RNDIS 天然是 L2（承载完整以太帧），
而 `feth` + BPF 是唯一能在纯用户态双向收发**原始以太帧**的路径。这样手机侧的
DHCP 服务器与 ARP 广播直接对主机 IP 栈生效，我们完全不需要自己实现 ARP 代答、
邻居发现或 DHCP 客户端 —— 那三样才是 L3 方案真正的复杂度所在。

代价：需要 root（`feth` 创建与 `/dev/bpf*` 都要），每方向多一次 mbuf 拷贝，
TX 有「整帧 ≤ MTU + 18」的长度限制。

---

## 2. 总体架构

```
        ┌──────────────────────── macOS 内核 ────────────────────────┐
        │  IP 栈 / 路由 / DHCP 客户端                                 │
        │       │                                                    │
        │       ▼                                                    │
        │  ┌─────────┐   if_fake peer 对   ┌─────────┐               │
        │  │  feth0  │ ◄─────────────────► │  feth1  │               │
        │  │(系统侧) │                     │(驱动侧) │               │
        │  └─────────┘                     └────┬────┘               │
        │   配 IP/路由                          │ BPF（单个描述符    │
        │   MAC = 设备汇报的地址                 │  同时收发）        │
        └───────────────────────────────────────┼────────────────────┘
                                                │
        ┌───────────────────────────────────────┼────────────────────┐
        │  TetherKitNext（用户态）                   ▼                    │
        │  ┌────────────── core::Bridge（3 个数据路径线程）────────┐  │
        │  │  RX: bulk IN 回调 → FrameRing → BPF 批量 write        │  │
        │  │  TX: BPF 批量 read → RNDIS 多包聚合 → bulk OUT        │  │
        │  └──────────────────────┬───────────────────────────────┘  │
        │  ┌───────── rndis::StateMachine（控制线程）─────────┐       │
        │  │  INITIALIZE / QUERY / SET / KEEPALIVE / RESET /  │       │
        │  │  HALT / INDICATE_STATUS                          │       │
        │  └──────────────────────┬──────────────────────────┘       │
        │  ┌───────── usb::Device / DataChannel ─────────────┐       │
        │  │  控制通道（EP0 类请求）+ 中断 IN 通知             │       │
        │  │  异步 bulk IN/OUT 传输池                         │       │
        │  └──────────────────────┬──────────────────────────┘       │
        └─────────────────────────┼──────────────────────────────────┘
                                  │ USB
                             ┌────┴─────┐
                             │ RNDIS 设备 │
                             └──────────┘
```

### 模块与依赖方向

依赖**严格单向**，禁止反向或循环：

```
tk_common  ←（无依赖）      错误、日志、字节序、无锁队列、统计、线程 QoS
   ↑
tk_rndis   ← common         RNDIS 线格式 + 状态机（**纯逻辑，不碰 I/O**）
tk_net     ← common         feth 生命周期 + BPF 链路
tk_usb     ← common, rndis  libusb 封装（rndis 只用来拿协议常量）
   ↑
tk_core    ← 以上全部        数据路径桥接 + 运行时编排
   ↑
tetherkitnext  ← core           命令行
```

**`tk_rndis` 不碰 I/O 是刻意的约束**，它让整个 RNDIS 实现（包括状态机）能在
一台既没有 USB 设备也没有 root 的机器上被完整单元测试 —— 而本项目的开发机正是
这样一台机器。为此 `ControlChannel` 接口放在 `rndis` 层（它描述的是 RNDIS 协议
语义，不是 USB 语义），由 `tk_usb` 提供实现。

---

## 3. 数据流向语义（最关键、也最容易搞错的一点）

已对照 xnu 的 `feth_output_common()` 源码确认：

| 事件 | 在 feth0 上 | 在 feth1 上 |
|---|---|---|
| 主机 IP 栈从 feth0 发出一帧 | **OUT** tap | **IN** tap，并作为 input 进入 |
| 我们向 feth1 的 BPF `write()` 一帧 | 作为 **input** 进入 IP 栈 | **OUT** tap |

由此得到两个结论：

1. **BPF 只需要挂在 feth1（驱动侧）上，一个描述符同时完成收和发。**
2. **必须设 `BIOCSSEESENT = 0`**（→ `bd_direction = BPF_D_IN`），它恰好滤掉
   「我们自己 `write` 进去的帧」（那些在 feth1 上是 OUT 方向）。不设就会形成回环，
   自己写的帧立刻被自己读回来。

---

## 4. 并发模型

三个数据路径线程，外加 libusb 自己的 IOKit runloop 线程与状态机的控制线程。
本机 4 性能核 + 6 效率核，三个热线程 + libusb runloop 刚好占满性能核集群
且不过订阅。

| 线程 | 职责 | 阻塞点 | QoS |
|---|---|---|---|
| `usb-event` | `libusb_handle_events`，跑所有 transfer 回调 | event pipe | USER_INTERACTIVE |
| `rx-inject` | 从 `FrameRing` 批量取帧 → BPF 批量 `write` | 队列空时自旋后 yield | USER_INTERACTIVE |
| `tx-extract` | BPF 阻塞 `read` → RNDIS 聚合 → 异步 bulk OUT | BPF `read()` | USER_INTERACTIVE |
| `rndis-ctl` | 状态机 `Poll()`：保活 + 排空推送 | `sleep` | USER_INITIATED |

### 为什么 RX 必须拆成两个线程

libusb 的 transfer 回调持有 `ctx->event_waiters_lock`，回调里做阻塞 I/O 会把
**所有等同步传输的线程**（包括跑控制通道的状态机线程）一起拖住。而 BPF 的
`write()` 是系统调用（约 700 ns 起）。所以回调只做
「拆 RNDIS 包 + memcpy 进无锁队列 + 立刻 resubmit」，真正的写出交给注入线程。

### 为什么 TX 只需要一个线程

BPF 的 `read()` 本身就是阻塞且自动聚合的（`BIOCIMMEDIATE=1` 下每来一包就唤醒，
醒来时一次性交付期间累积的全部包），而 `SendFrames` 是**异步**提交、不阻塞。
「读一批 → 提交一批」在同一个线程里就是最优形态，多一个线程只会多一次跨核传递。

### 为什么不用单个 kqueue 事件循环合并三件事

调研实测：macOS 上一次空的 `kevent()`（timeout={0,0}、无事件）要 **13.4~13.7 µs**,
比普通系统调用贵 20 倍。25 kpps 下光探测就要吃掉 34% 的单核。
**kqueue 只能用于真正的阻塞等待，绝不能用于轮询** —— 而我们三条路径各自都已经有
天然的阻塞点，合并没有收益。

### 为什么控制通道必须独占一个非事件线程

libusb 的同步 API（`libusb_control_transfer` / `libusb_interrupt_transfer`）
开头就是 `if (usbi_handling_events(ctx)) return LIBUSB_ERROR_BUSY;` ——
这是个 TLS 判断，从事件线程（含任何 transfer 回调内部）调用必然失败。
所以状态机不能放进事件循环，必须有自己的线程。`main` 把主线程给了它。

### 为什么不用 `THREAD_TIME_CONSTRAINT_POLICY`

它的语义是「每 period 保证 computation 的 CPU」，适合严格周期性的音频渲染线程。
我们三个线程都长时间阻塞在 `read()` / `write()` / `handle_events()` 上，
设 TC 会（a）清掉线程的 QoS class，（b）超支 computation 时被内核降级。
另外 macOS **没有** CPU 亲和性 API（`thread_affinity_policy` 在 Apple Silicon 上
基本无效），所以「绑核」不可行，只能调优先级。

---

## 5. 性能设计

完整数字见 [BENCHMARKS.md](BENCHMARKS.md)。核心结论：

> **优化目标是每帧的系统调用次数，不是 CPU 周期。**

每帧的 CPU 成本合计约 70 ns（RNDIS 编码 ~15 + 队列搬运 ~55 + 统计 ~0.5），
而一次系统调用约 700 ns —— **系统调用是 CPU 的 10 倍**。macOS 上系统调用比
Linux 贵 3~10 倍，这是整个设计的第一约束。

由此推出四条措施：

| 措施 | 机制 | 效果 |
|---|---|---|
| RX 批量写 | `BIOCSBATCHWRITE`（macOS 14+，特性探测，失败回落逐帧） | 一批帧一次系统调用 |
| TX 多包聚合 | RNDIS 允许一次 bulk OUT 承载多个 `PACKET_MSG` | 一批帧一次 USB 往返 |
| 让设备也聚合 | `INITIALIZE_MSG` 里把 `MaxTransferSize` 报大（默认 16 KiB） | **RX 吞吐的主要杠杆** |
| 批量发布队列 | `FrameRing::BatchWrite/BatchRead`，一批只做一次 release store | 1514 字节帧提速 2.4 倍，64 字节 5.0 倍 |

### 为什么只拷贝一次、而不追求零拷贝

RX 路径每帧一次 memcpy（USB 缓冲 → `FrameRing` 槽位）。追求零拷贝要求把 USB
传输缓冲交给下游持有，就不能立刻 resubmit，得准备远多于「在飞传输数」的缓冲
并引入归还机制 —— 而 memcpy 1514 字节只要 25~70 ns，是系统调用的 1/10。
**为它做零拷贝是错误的优化方向。**

TX 方向的零拷贝更是**根本不可行**：`struct bpf_hdr` 只有 20 字节，而 RNDIS 包头
需要 44 字节，帧前空间差 24 字节；feth 的 `tx_headroom` 是 32 字节，同样不够。

### 缓存行是 128 字节

`sysctl hw.cachelinesize` 报 **128**（不是习惯性的 64）。按 64 对齐的话，
生产者与消费者的索引仍会落在同一条 128 字节缓存行上，false sharing 依然存在。

刻意**不用** `std::hardware_destructive_interference_size`：它在 Apple libc++ 上
确实存在（在 `<new>` 里），但报的是 **256**，与真实的 128 不符 —— 用它会把所有
填充翻倍。

---

## 6. 错误处理

| 路径 | 手段 | 理由 |
|---|---|---|
| 初始化 / 控制 | `std::expected<T, Error>` | 失败绝大多数是「预期内的环境问题」（没插设备、没有 root、接口被占），调用方总要处理，不该靠异常 |
| **数据热路径** | **返回计数 + 原子计数器** | `Error` 里的 `std::string` 会分配堆内存，25k~80k pps 下不可接受 |

`Error` 携带来源域（errno / libusb / RNDIS_STATUS / 逻辑），`ToString()` 负责把
「libusb 返回 -3」翻译成「LIBUSB_ERROR_ACCESS(-3)」。`WithContext` 支持叠加外层
原因形成「外层：内层」的链条 —— 那个分隔符随语言变化（中文全角冒号、英文
`": "`），所以由 `detail::ContextSeparator()` 提供，不写死在 `error.h` 里。

错误消息本身全部来自文案表（见第 6b 节），不是字面量。

**RNDIS 状态码到名字的映射只存在于 `rndis/protocol.cc` 一处**（单一来源）；
`tk_common` 不认识 RNDIS，只输出十六进制数值，符号名由 rndis 层拼进上下文串。
（此前在 `error.cc` 里也放了一份，5 个数值是错的，已删除。）

---

## 6b. 文案与多语言

面向用户的文字（错误、日志、帮助、状态名）**一条都不写字面量**，全部集中在
`include/tetherkitnext/common/messages.def`。该文件是一份 X-macro 清单，被展开三次
分别生成 `Msg` 枚举、中文表与英文表 —— 三者同源，**结构上不可能出现某种语言
漏了一条**。

| 取用方式 | 用途 | 是否分配 |
|---|---|---|
| `Tr(Msg::kFoo, args...)` | 带参数，语义同 `std::format` | 会（返回 `std::string`） |
| `Text(Msg::kFoo)` | 不带参数，返回 `string_view` | 不会，`noexcept` |
| `TETHERKITNEXT_INFO_TR(Msg::kFoo, ...)` 等 | 打日志 | 级别没开时不求值 |

**为什么不用 gettext**：要引入 libintl、构建期跑 msgfmt、运行期按路径查目录，
换来的是「装到别的机器上找不到 `.mo` 于是全变英文」这类运行期故障。两种语言、
几百条固定文案，编译进二进制的一张表最省事。

**代价与对策**：格式串变成运行期查表，`std::format` 的编译期占位符校验就没了
（`Tr` 内部走 `vformat`）。这道保障由 `tests/test_common_i18n.cc` 补回来 ——
它遍历整张表，逐条比对两种语言引用的参数下标与表现类型，并检查下标连续、
未在同一串里混用自动与手工编号。占位符写错会在 `ctest -R common.i18n` 当场红掉，
而不是等到某条日志在用户机器上渲染成半句话。

`Tr()` 和 `error.h` 一样**禁止出现在数据热路径上**（它必然分配）。热路径需要
文字时只能用 `Text()`。

语言由宿主决定：命令行看 `--lang` 与环境变量，GUI 通过 C ABI 的
`tk_set_language` 推进来。这是**进程级单一状态**，不是每个会话一份 —— 日志与
错误从多个线程产生，做成线程局部只会让同一次会话的输出出现两种语言。

---

## 7. 背压策略

| 位置 | 满了怎么办 | 理由 |
|---|---|---|
| RX 队列（USB → BPF） | **丢弃并计数** | libusb 回调里不能阻塞（持锁），只能丢 |
| TX 传输池（BPF → USB） | **丢弃并计入背压事件** | 等待会让 BPF 内核缓冲堆积，最终由内核丢 —— 那是运维**看不见**的丢包。在用户态丢并计数才能定位瓶颈。TCP 会重传，UDP 本来允许丢 |
| 暂停期间的 RX | **不丢**，队列里的帧等恢复后继续送 | 链路只是暂时 down，帧仍然有效 |
| 暂停期间的 TX | **丢弃并计数** | RNDIS 软复位期间设备会丢弃所有未完成的数据包，攒着只是浪费内存 |

---

## 8. 拆除顺序（避免 use-after-free）

`libusb_close` **不会**回收在飞 transfer —— 它只是 `list_del` + 把
`dev_handle` 置空并打一条日志，**不调回调、不释放内存**；之后 IOKit 中止仍会让
`darwin_async_io_callback` 在 libusb 内部线程上跑。若那时已 `free` 就是 UAF。
这是 libusb 用法里最常见的崩溃源。

正确顺序（`Runtime::Stop()` 与 `UsbDataChannel::Shutdown()` 共同实现）：

```
1. Bridge::Stop()
     ├ 置停机标志
     ├ link->Interrupt()        打断 BPF 阻塞读
     ├ join rx-inject / tx-extract
     └ DataChannel::Shutdown()
          ├ 置停机标志（回调不再 resubmit）
          ├ cancel 全部在飞 transfer
          ├ **等在飞计数归零**（每个回调都回来了）
          └ 才 libusb_free_transfer + 释放缓冲
2. 关 BPF
3. 销毁 feth 网卡对
4. 状态机 Stop()：SET filter=0 → HALT
5. 释放 USB 接口、关闭句柄
6. 停 libusb 事件线程、libusb_exit
```

两个必须记住的约束：

- **第 1 步里「等在飞计数归零」绝不能在 libusb 事件线程上等** —— 递减它的回调
  正是在那个线程上跑的，在那里等就是自己等自己，必然死锁。
- 等待超时时**故意泄漏**内存而不是冒险释放。泄漏几百 KB 远好于 UAF 崩溃，
  并打出明确的错误日志说明这一决定。

---

## 9. 可测试性

开发机既**没有 USB 设备**（`libusb_get_device_list` 返回 0）也**没有 root**。
因此每个需要外部资源的边界都做了接口抽象：

| 接口 | 生产实现 | 测试实现 |
|---|---|---|
| `rndis::ControlChannel` | `usb::UsbControlChannel` | `testing::MockControlChannel` |
| `usb::DataChannel` | `usb::UsbDataChannel` | `testing::MockDataChannel` |
| `net::LinkBackend` | `net::BpfLink` | `net::LoopbackLink` |

**抽象的粒度是「批」而不是「帧」**：一次虚调用处理几十上百帧，摊到每帧的开销
远小于 1 ns。如果做成每帧一次虚调用就不可接受了 —— 那是刻意避开的设计。

于是状态机的全部路径（含设备插队推送、保活失败、复位重放）、桥接层的全部路径
（含双向并发、背压、暂停、有流量时停机）都能离线测试，并在 ThreadSanitizer 下
验证。需要 root 的 feth/BPF 用例默认**跳过而非失败**，用
`TETHERKITNEXT_ROOT_TESTS=1` 显式开启。

---

## 10. 私有 ABI 的使用与风险

本项目用到三处不在公开 SDK 里的东西：

| 私有 ABI | 用途 | 风险与对策 |
|---|---|---|
| `struct ifdrv` | `SIOCSDRVSPEC` 的参数 | 大小参与 ioctl 编号计算，算错只会得到不存在的 ioctl（返回 `ENOTTY`，不会做危险的事）。用 `static_assert` 把大小=40、各字段偏移、以及推导出的 `SIOCSDRVSPEC=0x8028697b` 全部钉死 |
| `struct if_fake_request` | feth peer 配对 | 该文件在 xnu-7195(macOS 11) → xnu-12377(macOS 26) 的所有发布 tag 下内容完全相同，且 Apple 自己的 `ifconfig fethN peer fethM` 就依赖它 —— 实际上已被冻结。同样有 `static_assert`（大小=160） |
| `BIOCSBATCHWRITE` / `BIOCSNOTSTAMP` | 性能优化 | **纯可选**。一律做运行时特性探测，失败回落到通用路径，不影响功能正确性 |

一句话：**功能性 ABI 经过 15 年验证且有 `static_assert` 兜底；优化性 ABI 做特性
探测。两者都不会静默走错路径。**

---

## 11. 代码规范

| 类别 | 约定 | 例 |
|---|---|---|
| 命名空间 | `lower_case` | `tetherkitnext::rndis` |
| 类型 | `CamelCase` | `PacketMessageWriter` |
| 函数与方法（含访问器） | `CamelCase` | `MaxFrameBytes()` |
| 局部变量与参数 | `lower_case` | `frame_length` |
| 私有成员 | `lower_case_` | `bulk_in_endpoint_` |
| 常量 / 枚举值 | `k` + `CamelCase` | `kPacketMsgHeaderBytes` |
| 文件 | `snake_case`，`.h` / `.cc` | `packet_codec.h` |
| 文案标识 | `k` + 模块前缀 + `CamelCase` | `kNetBpfBindFailed`、`kCliUnknownOption` |

访问器也用 `CamelCase`（而非标准库风格的 `lower_case`），是为了让
`readability-identifier-naming` 能机械地全量检查，不留「凭记忆遵守」的例外。

`.clang-tidy` 里关掉了一批对底层系统编程不适用的检查，每一条都写了理由。
特别是 `performance-enum-size` 必须关掉 —— 本项目的协议枚举底层类型**必须**
精确等于线格式的字段宽度（RNDIS 全是 LE32），按它的建议缩小会直接写坏线格式。
