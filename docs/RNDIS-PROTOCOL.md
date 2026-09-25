# RNDIS 协议参考

本文是实现 RNDIS **主机侧**时需要的全部线格式细节。所有数值都与
[`include/tetherkitnext/rndis/protocol.h`](../include/tetherkitnext/rndis/protocol.h)
一致（那里是代码中的唯一来源），并与 Linux 内核实现交叉验证过：
`drivers/net/usb/rndis_host.c`、`include/linux/usb/rndis_host.h`、
`drivers/usb/gadget/function/rndis.c`。

---

## 0. 三条最容易搞错的规则

### ① 所有多字节字段都是**小端**

协议源自 Windows NDIS。禁止用 packed struct 直接映射线格式 —— 一是大端机上会
静默出错，二是 RNDIS 消息在 USB 缓冲里的起始偏移只保证 4 字节对齐，而
`PACKET_MSG` 之后紧跟的以太帧起始偏移由设备的 `DataOffset` 决定，可能是任意值。
本项目全部走 `memcpy` + 显式字节序转换（clang 会优化成单条 `ldr`/`rev`，
实测 0.10 ns/字段，零成本）。

### ② 偏移字段的基准点是**消息起始 + 8**，不是消息起始

`PACKET_MSG` 的 `DataOffset` / `OOBDataOffset` / `PerPacketInfoOffset`，
以及 `QUERY` / `SET` / `QUERY_CMPLT` 的 `InformationBufferOffset`，
基准点都是「该组偏移字段所属区块的起始」，即消息起始 + 8 字节处：

```
绝对偏移 = 8 + 字段值
```

所以「以太帧紧跟 44 字节 `PACKET_MSG` 头」时，`DataOffset` 要填 **36**，不是 44。
**这是实现 RNDIS 的第一号陷阱。** 代码里用 `static_assert` 钉死了：

```cpp
inline constexpr std::uint32_t kPacketInlineDataOffset =
    kPacketMsgHeaderBytes - kOffsetFieldBase;
static_assert(kPacketInlineDataOffset == 36, "DataOffset 基准点是消息起始 +8");
```

### ③ `INDICATE_STATUS` 的 `StatusBufferOffset` 是例外

Microsoft 文档写它的基准点是**消息起始**（+0），与上面的 +8 约定**不一致**。
这是规范自身的矛盾。实践中该字段绝大多数为 0（`MEDIA_CONNECT` /
`MEDIA_DISCONNECT` 不带 status buffer），因此本项目的解析器按以下顺序容错：

1. 长度为 0 → 无负载，直接成功（绝大多数情况）；
2. 先按「基准点 = 消息起始 + 8」解释，落在消息范围内则采用；
3. 否则按「基准点 = 消息起始」解释；
4. 两种都越界 → 丢弃状态缓冲区但**仍返回成功** —— `status` 本身有用
   （可能就是 `MEDIA_DISCONNECT`），不能因为一个可选负载解析不了就把链路判死。

---

## 1. 消息类型码

| 消息 | 码值 | 备注 |
|---|---|---|
| `REMOTE_NDIS_PACKET_MSG` | `0x00000001` | 数据通道 |
| `REMOTE_NDIS_INITIALIZE_MSG` | `0x00000002` | |
| `REMOTE_NDIS_HALT_MSG` | `0x00000003` | **设备不回复** |
| `REMOTE_NDIS_QUERY_MSG` | `0x00000004` | |
| `REMOTE_NDIS_SET_MSG` | `0x00000005` | |
| `REMOTE_NDIS_RESET_MSG` | `0x00000006` | **无 RequestId** |
| `REMOTE_NDIS_INDICATE_STATUS_MSG` | `0x00000007` | 设备发起，**无 RequestId、无需回复** |
| `REMOTE_NDIS_KEEPALIVE_MSG` | `0x00000008` | **双向都可发起** |
| 完成消息 | 请求码 \| `0x80000000` | |
| `RNDIS_MSG_BUS` | `0xFF000001` | 私有，本项目仅识别并忽略 |

面向连接（CONDIS）消息族 `0x00008001`~`0x00008007`：802.3 设备不会用到。
本项目识别它们只为了能明确报「不支持面向连接设备」而不是含糊的「未知消息」。

---

## 2. 消息长度与字段偏移

所有字段都是 LE32。

| 消息 | 总长 | 字段偏移 |
|---|---:|---|
| `INITIALIZE_MSG` | 24 | Type@0, Length@4, RequestId@8, MajorVersion@12, MinorVersion@16, MaxTransferSize@20 |
| `INITIALIZE_CMPLT` | 52 | …, Status@12, MajorVersion@16, MinorVersion@20, DeviceFlags@24, Medium@28, **MaxPacketsPerMessage@32**, **MaxTransferSize@36**, **PacketAlignmentFactor@40**, AFListOffset@44, AFListSize@48 |
| `HALT_MSG` | 12 | …, RequestId@8 |
| `QUERY_MSG` = `SET_MSG` | 28 + 负载 | …, RequestId@8, Oid@12, InfoBufferLength@16, InfoBufferOffset@20, DeviceVcHandle@24 |
| `QUERY_CMPLT` | 24 + 负载 | …, Status@12, InfoBufferLength@16, InfoBufferOffset@20 |
| `SET_CMPLT` | 16 | …, Status@12 |
| `RESET_MSG` | 12 | …, **Reserved@8**（⚠️ 不是 RequestId） |
| `RESET_CMPLT` | 16 | …, **Status@8**（⚠️ 不是 12）, **AddressingReset@12** |
| `INDICATE_STATUS_MSG` | 20 + 负载 | …, Status@8, StatusBufferLength@12, StatusBufferOffset@16 |
| `KEEPALIVE_MSG` | 12 | …, RequestId@8 |
| `KEEPALIVE_CMPLT` | 16 | …, RequestId@8, Status@12 |
| `PACKET_MSG` 头 | 44 | Type@0, Length@4, **DataOffset@8**, DataLength@12, OOBDataOffset@16, OOBDataLength@20, NumOOBDataElements@24, PerPacketInfoOffset@28, PerPacketInfoLength@32, VcHandle@36, Reserved@40 |

「数据紧跟头部」时偏移字段该填的值：

| 消息 | 头长 | 偏移字段值 |
|---|---:|---:|
| `PACKET_MSG` | 44 | **36** |
| `QUERY_MSG` / `SET_MSG` | 28 | **20** |
| `QUERY_CMPLT` | 24 | **16** |

---

## 3. 状态码

关键的几个（完整列表见 `protocol.h` 的 `StatusCode`）：

| 名字 | 值 | 说明 |
|---|---|---|
| `SUCCESS` | `0x00000000` | |
| `PENDING` | `0x00000103` | |
| `MEDIA_CONNECT` | `0x4001000B` | 链路 up |
| `MEDIA_DISCONNECT` | `0x4001000C` | 链路 down |
| `LINK_SPEED_CHANGE` | `0x40010013` | |
| `FAILURE` | `0xC0000001` | |
| `NOT_SUPPORTED` | `0xC00000BB` | 可选 OID 的正常回应 |
| `INVALID_LENGTH` | `0xC0010014` | ⚠️ 易记错 |
| `INVALID_DATA` | `0xC0010015` | ⚠️ 易记错 |
| `BUFFER_TOO_SHORT` | `0xC0010016` | |
| `INVALID_OID` | `0xC0010017` | ⚠️ 易记错 |

> 这四个带 ⚠️ 的值本项目最初在 `error.cc` 里抄错过（写成了 `0xC000000D` /
> `0xC0010015` / `0xC0000010` / `0x80000005`）。现在状态码表只存在于
> `rndis/protocol.cc` 一处，避免两处不一致。

失败判定：`(status & 0xC0000000) == 0xC0000000`。

---

## 4. OID

| OID | 值 | 数据格式 | 本项目用途 |
|---|---|---|---|
| `OID_GEN_SUPPORTED_LIST` | `0x00010101` | **变长**：N 个 LE32 | 未使用 |
| `OID_GEN_MAXIMUM_FRAME_SIZE` | `0x00010106` | LE32，**不含**以太头 | 校验并可能下调 MTU |
| `OID_GEN_LINK_SPEED` | `0x00010107` | LE32，**单位 100 bps** | 日志 |
| `OID_GEN_VENDOR_ID` | `0x0001010C` | LE32，低 24 位是 OUI | 日志 |
| `OID_GEN_VENDOR_DESCRIPTION` | `0x0001010D` | **变长** ASCII，**不保证 NUL 结尾** | 日志 |
| `OID_GEN_CURRENT_PACKET_FILTER` | `0x0001010E` | LE32 位掩码，可读可写 | **设非零值才让数据流动** |
| `OID_GEN_MEDIA_CONNECT_STATUS` | `0x00010114` | LE32：**0=已连接**，1=断开 | 初始链路状态 |
| `OID_GEN_PHYSICAL_MEDIUM` | `0x00010202` | LE32，**可选 OID** | 日志（失败不致命） |
| `OID_802_3_PERMANENT_ADDRESS` | `0x01010101` | 6 字节 MAC | **主机侧 feth 采用的地址** |
| `OID_802_3_CURRENT_ADDRESS` | `0x01010102` | 6 字节 MAC | 日志 |
| `OID_802_3_MULTICAST_LIST` | `0x01010103` | SET：6N 字节 | 未使用（包过滤里开了 ALL_MULTICAST） |

⚠️ **`OID_GEN_MEDIA_CONNECT_STATUS` 的 0 表示「已连接」**，不是断开。

⚠️ **不要依赖 `QUERY OID_802_3_MULTICAST_LIST`**：Linux gadget 对它返回一个
4 字节的 `0xE0000000`，不符合「6 字节 MAC 数组」的格式。只用 SET 方向。

### `InformationBufferLength` 的反直觉规则

这条来自嗅探微软 ActiveSync 4.1 的 Windows 驱动，**未文档化**：

| OID 类型 | `InformationBufferLength` | 违反后果 |
|---|---|---|
| 返回**定长**结果 | 必须 ≥ 期望响应长度，且消息尾部要真的有那么多字节 | `RNDIS_STATUS_INVALID_LENGTH` |
| 返回**变长**结果 | 必须为 **0** | 同样出错 |

本项目的 `Encode(QueryRequest)` 按 `IsVariableLengthOid()` 自动处理，
调用方不必操心。测试里有专门的用例把这条规则钉住。

---

## 5. 包过滤位掩码

| 名字 | 值 |
|---|---|
| `DIRECTED` | `0x00000001` |
| `MULTICAST` | `0x00000002` |
| `ALL_MULTICAST` | `0x00000004` |
| `BROADCAST` | `0x00000008` |
| `PROMISCUOUS` | `0x00000020` |

本项目用 `DIRECTED | BROADCAST | ALL_MULTICAST | PROMISCUOUS` = **`0x0000002D`**
（与 Linux `rndis_host` 一致）。

开 `PROMISCUOUS` 的理由：我们把设备当成一根「网线」桥接到 feth，主机侧可能收发
任意 MAC 的帧（上层再做桥接或跑虚拟机时）。开 `ALL_MULTICAST` 的理由：避免维护
组播表还漏掉 IPv6 邻居发现。

---

## 6. 主机侧状态机

规范定义的是**设备侧**三态，主机侧镜像它并补上过渡态：

```
kUninitialized ──Start()/INITIALIZE──► kInitializing ──CMPLT+协商通过──► kInitialized
      ▲                                     │                              │
      │                                  协商失败                    SET filter≠0
      │                                     │                              ▼
      └──────────── HALT / 断开 ────────────┴──────────────────── kDataInitialized
                                                                           │
                                                              SET filter=0 │ Stop()
                                                              （退回上态）  ▼
                                                                      kHalting
```

规范原文语义：

- 总线初始化后设备处于 RNDIS-uninitialized；
- 收到 `INITIALIZE_MSG` 并回 `INITIALIZE_CMPLT(SUCCESS)` → RNDIS-initialized；
- 收到 `SET(OID_GEN_CURRENT_PACKET_FILTER, **非零**)` → RNDIS-data-initialized，
  **数据这才开始流动**；
- data-initialized 下收到 `SET(filter = 0)` → 退回 RNDIS-initialized；
- 任意时刻收到 `HALT_MSG` 或总线断开 → 立即回 RNDIS-uninitialized。

### 启动序列（照 Linux 的 `generic_rndis_bind`）

| 步 | 操作 | 致命？ |
|---|---|---|
| 1 | `INITIALIZE_MSG` → `INITIALIZE_CMPLT`，协商参数 | ✔ |
| 2 | `QUERY OID_GEN_PHYSICAL_MEDIUM`（`in_len=4`） | ✘ 可选 OID |
| 3 | `QUERY OID_802_3_PERMANENT_ADDRESS`（`in_len=48`） | ✔ |
| 4 | `QUERY` 当前 MAC / 最大帧长 / 链路速率 / 连接状态 / 厂商信息 | ✘ |
| 5 | `SET OID_GEN_CURRENT_PACKET_FILTER = 0x2D` | ✔ |

任一致命步骤失败都会发 `HALT_MSG` 再返回错误，且状态回到未初始化而不是卡在中间态。

### 三条容易写错的交互细节

**① 控制请求必须串行化。**
`GET_ENCAPSULATED_RESPONSE` 只能「取下一条排队的响应」，没有按 `RequestId`
选取的能力。所以同一时刻只能有一个请求在飞。

**② 设备会插队。**
在等某个 `*_CMPLT` 的过程中，设备完全可能先塞进来一条 `INDICATE_STATUS_MSG`
（媒体连接状态变化），或一条**设备发起的** `KEEPALIVE_MSG` —— 此时主机
**必须回** `KEEPALIVE_CMPLT`，否则设备可能认为主机已死而断开。
因此控制读循环必须写成 dispatcher（按 `MessageType` 分派），而不是
「盲目假定读到的就是我要的那条」。

**③ `RESET` 的语义。**
`RESET` 是 soft reset：控制通道保持完整，但设备丢弃**所有**未完成的请求与数据包。
`RESET_CMPLT` 的 `AddressingReset` 非零表示寻址信息（包过滤、组播表）在复位中
丢失，主机**必须重发** `SET(OID_GEN_CURRENT_PACKET_FILTER)` —— 否则复位后数据
不会再流动。Linux gadget 无条件把 `AddressingReset` 置 1。

---

## 7. USB 层

### 控制通道

| 请求 | `bmRequestType` | `bRequest` | `wValue` | `wIndex` |
|---|---|---|---|---|
| `SEND_ENCAPSULATED_COMMAND` | `0x21`（OUT\|Class\|Interface） | `0x00` | 0 | 通信类接口号 |
| `GET_ENCAPSULATED_RESPONSE` | `0xA1`（IN\|Class\|Interface） | `0x01` | 0 | 通信类接口号 |

⚠️ **设备尚无有效响应时，规范要求它返回 1 字节 `0x00`，而不是 STALL 控制端点。**
因此主机收到 < 8 字节（一个 RNDIS 消息头）的结果必须当作「还没准备好，稍后重试」，
**不能当作错误**。

### 中断 IN 通知

8 字节 = 两个 LE32：`Notification`@0（`0x00000001` = RESPONSE_AVAILABLE）、
`Reserved`@4。

⚠️ 这**不是** CDC 的 `usb_cdc_notification` 结构，而是 RNDIS 自有格式。

设备行为有三类，实现必须都兼容：

1. 规范做法：主机等中断 IN 上的通知；
2. Linux 做法：**完全忽略**中断端点，直接对 `GET_ENCAPSULATED_RESPONSE`
   轮询最多 10 次、每次间隔 40 ms；
3. 反过来还存在**必须先被中断端点读过一次才会在控制端点上作答**的设备。

本项目的策略：有中断端点就先等一次通知（超时也不算错），然后**无论如何**都去轮询。

### 接口签名（四种已知形态）

| 形态 | 通信类接口 | 数据类接口 |
|---|---|---|
| 标准 MS RNDIS | `0x02 / 0x02 / 0xFF` | `0x0A / 0x00 / 0x00` |
| ActiveSync（Windows Mobile / Phone） | `0xEF / 0x01 / 0x01` | 同上 |
| 无线 RNDIS（手机共享、WWAN 模块） | `0xE0 / 0x01 / 0x03` | 同上 |
| Novatel/Verizon USB730L 变体 | `0xEF / 0x04 / 0x01` | 同上 |

**排除伪 RNDIS**：class == `0x02` 且带**非零** CDC ACM `bmCapabilities` 的是真
cdc-acm 调制解调器，不是 RNDIS。⚠️ **这条检查只能对 class `0x02` 生效** ——
无线类（`0xE0`）的 RNDIS function 会把 `bmCapabilities` 字段挪作自用。

---

## 8. 多包聚合与 ZLP

### 聚合规则

- **device → host**：每个 `PACKET_MSG` 应从「多包消息起始」的 8 字节整数倍偏移开始；
- **host → device**：必须遵守设备在 `INITIALIZE_CMPLT` 里给的
  `PacketAlignmentFactor`，对齐字节 = `1 << factor`，**合法上界 7**（=128 字节）；
- **`MessageLength` 包含填充** —— 除最后一个之外的每个 `PACKET_MSG` 的
  `MessageLength` 都含尾部对齐填充，最后一个**不含**外部填充；
- 单次传输总字节数 ≤ 对端的 `MaxTransferSize`，包数 ≤ `MaxPacketsPerMessage`；
- 遍历方式：`offset += MessageLength`。

本项目的编码器实现「填充归入**上一个**消息」的做法：在追加下一个消息时才回头
扩大上一个消息的 `MessageLength` 以吞掉中间的填充 —— 这样最后一个消息的
`MessageLength` 天然就是 `44 + 帧长`，完全符合规范。

### ZLP 规避

RNDIS 明确要求主机**不得**发零长度包（`rndis_host.c` 顶部注释原文就是
"DATA -- host must not write zlps"）。而当传输长度恰为端点 `wMaxPacketSize` 的
整数倍时，USB 主机控制器需要一个短包来标记传输结束。

Linux `usbnet` 的做法（本项目照做）：**追加 1 个 `0x00` 字节**，让传输变成短包。
这个字节落在所有 `MessageLength` 之外，设备必须容忍尾部垃圾。

⚠️ 反过来，**主机的 RX 解析器也必须容忍 `actual_length > MessageLength`**。
不容忍的话，每个满传输都会误报一次畸形。本项目的解析器把「剩余不足一个完整
44 字节头部」的尾部一律当填充忽略。

⚠️ `LIBUSB_TRANSFER_ADD_ZERO_PACKET` 在 macOS 上**不可用**（头文件明确写非 Linux
系统会返回 `LIBUSB_ERROR_NOT_SUPPORTED`）。本项目不依赖它。

---

## 9. `MaxTransferSize` —— 吞吐的主要杠杆

主机在 `INITIALIZE_MSG` 里宣称的 `MaxTransferSize` 是**让设备把多个
`PACKET_MSG` 聚合进一次 bulk IN 的唯一手段**。报得越大、设备聚合越多、每帧摊到
的 USB 与系统调用开销越低。

Linux 的算法很保守（只报一个满帧）：

```
hard_header_len = ETH_HLEN(14) + sizeof(rndis_data_hdr)(44) = 58
hard_mtu        = MTU + 58                    # MTU=1500 → 1558
rx_urb_size     = (hard_mtu + maxpacket + 1) & ~(maxpacket - 1)
                # 高速(512) → 2048；全速(64) → 1600
```

本项目默认报 **16 KiB**（可用 `--max-transfer-kb` 调），既留足聚合空间，
又不超过规范对 USB 1.1 设备的 `0x4000` 上限。bulk IN 缓冲会自动跟着放大 ——
它必须 ≥ 我们宣称的值，否则设备聚合出来的大传输会溢出/被截断。

---

## 10. 已知设备 quirk

| quirk | 现象 | 本项目的处理 |
|---|---|---|
| **Android CDC Union 损坏** | CDC Union 描述符指向不存在的接口号，或干脆缺少 CDC 功能描述符 | 回落到「接口 0 = control、接口 1 = data」的硬编码假设（同 Linux 的 `android_rndis_quirk`），并在日志里标明走了兜底路径 |
| **`MaxPacketsPerMessage = 0`** | 部分设备汇报 0 | 当 1 处理 |
| **高通上行聚合补丁** | `MaxPacketsPerMessage` 变成模块参数，默认 3 | 原样采用，TX 侧会聚合 3 包 |
| **`PacketAlignmentFactor` 越界** | 汇报 > 7 的值 | 钳到 7。不钳位的话 `1u << 31` 会算出荒谬的填充长度 |
| **`MaxTransferSize` 过小** | HTC Diamond 报 1536 < `hard_mtu`(1558) | 反推并下调 MTU 到 `1536 - 58 = 1478` |
| **`MaxTransferSize` 过大** | WinCE / Windows Mobile 习惯报 8 KB 或 16 KB 巨帧 | 钳到主机自己的缓冲上限，不盲从设备 |
| **主线 gadget 的典型值** | `MaxPacketsPerTransfer=1`、`MaxTransferSize=1580`、`PacketAlignmentFactor=0`、`DeviceFlags=CONNECTIONLESS`、`Medium=802_3` | 可作为测试基线（`tests/mock_control_channel.h` 里的 `MakeWellBehavedDevice` 就是按它造的） |

---

## 11. 超时常量

| 常量 | 规范值 | 本项目 | 理由 |
|---|---|---|---|
| `ControlTimeoutPeriod` | 10 秒 | **5 秒** | Linux 因 ActiveSync 的行为收缩到 5 秒；真要 10 秒才回，链路已经不可用 |
| `KeepAliveTimeoutPeriod` | 5 秒 | 5 秒 | 语义是「距上次从设备收到**任何**消息已过 5 秒」才发，不是无条件定时发 |
| 控制缓冲区 | ≥ 1024 字节 | **1536** | Windows 用 1025 这个奇怪的值，Linux 照抄；取 1536 留余量 |

⚠️ Apple Silicon 上 libusb 的**控制传输**比 x64 慢约 10 倍
（[libusb issue #1288](https://github.com/libusb/libusb/issues/1288)），
所以保活周期别调太小 —— 每次往返都是毫秒级预算。
