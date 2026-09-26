# RNDIS Protocol Reference

This document details all wire-format specifications needed to implement the **host side** of the RNDIS protocol. Every numeric constant matches [`include/tetherkitnext/rndis/protocol.h`](../include/tetherkitnext/rndis/protocol.h) (the single source of truth in code) and has been cross-checked against the Linux kernel implementations in `drivers/net/usb/rndis_host.c`, `include/linux/usb/rndis_host.h`, and `drivers/usb/gadget/function/rndis.c`.

---

## 0. Three Rules Most Commonly Misunderstood

### ① All Multi-Byte Fields Are **Little-Endian**

The protocol originates from Windows NDIS. Never map wire formats directly using packed C structs: first, doing so silently breaks on big-endian architectures; second, RNDIS messages inside USB buffers are only guaranteed 4-byte alignment, and the Ethernet frame immediately following a `PACKET_MSG` starts at whatever `DataOffset` the device specifies (which may have arbitrary alignment). TetherKitNext performs all wire access via `memcpy` + explicit byte-order conversion (which Clang optimizes into a single `ldr`/`rev` instruction, measured at 0.10 ns/field).

### ② Offset Fields Are Relative to **Message Start + 8**, Not Message Start

In `PACKET_MSG` (`DataOffset`, `OOBDataOffset`, `PerPacketInfoOffset`) as well as `QUERY`, `SET`, and `QUERY_CMPLT` (`InformationBufferOffset`), all offsets are measured from the start of the offset-bearing section — i.e., **8 bytes after the start of the message**:

```
absolute_offset = 8 + field_value
```

Therefore, when an Ethernet frame immediately follows the 44-byte `PACKET_MSG` header, `DataOffset` must be **36**, not 44. **This is the #1 pitfall in RNDIS implementations.** The codebase enforces this with a `static_assert`:

```cpp
inline constexpr std::uint32_t kPacketInlineDataOffset =
    kPacketMsgHeaderBytes - kOffsetFieldBase;
static_assert(kPacketInlineDataOffset == 36, "DataOffset is relative to message start + 8");
```

### ③ `StatusBufferOffset` in `INDICATE_STATUS` Is the Exception

Microsoft's specification defines `StatusBufferOffset` as relative to the **start of the message** (+0), contradicting the +8 rule used everywhere else. In practice, this field is almost always 0 (`MEDIA_CONNECT` and `MEDIA_DISCONNECT` carry no status buffer). Our parser handles this ambiguity defensively in the following order:

1. If `StatusBufferLength == 0` → no payload, succeed immediately (the vast majority of cases);
2. First interpret `StatusBufferOffset` relative to `message start + 8`; if it falls within `MessageLength`, use it;
3. Otherwise interpret it relative to `message start` (+0);
4. If both are out of bounds → discard the status buffer but **still return success**, because the `Status` code itself (e.g., `MEDIA_DISCONNECT`) is valid and essential, and an unparseable optional payload must not tear down the link.

---

## 1. Message Type Codes

| Message | Code | Notes |
|---|---|---|
| `REMOTE_NDIS_PACKET_MSG` | `0x00000001` | Data channel |
| `REMOTE_NDIS_INITIALIZE_MSG` | `0x00000002` | |
| `REMOTE_NDIS_HALT_MSG` | `0x00000003` | **No response from device** |
| `REMOTE_NDIS_QUERY_MSG` | `0x00000004` | |
| `REMOTE_NDIS_SET_MSG` | `0x00000005` | |
| `REMOTE_NDIS_RESET_MSG` | `0x00000006` | **No `RequestId`** |
| `REMOTE_NDIS_INDICATE_STATUS_MSG` | `0x00000007` | Device-initiated; **no `RequestId`, no response** |
| `REMOTE_NDIS_KEEPALIVE_MSG` | `0x00000008` | **Can be initiated by either side** |
| Completion messages | Request code \| `0x80000000` | |
| `RNDIS_MSG_BUS` | `0xFF000001` | Vendor/bus-private; recognized and ignored |

Connection-oriented (CONDIS) messages (`0x00008001`–`0x00008007`) are not used by 802.3 Ethernet devices. TetherKitNext recognizes their type codes solely to report a clear "connection-oriented RNDIS devices are not supported" error rather than a generic "unknown message".

---

## 2. Message Lengths and Field Offsets

All fields are 32-bit little-endian (`LE32`).

| Message | Total Bytes | Field Offsets |
|---|---:|---|
| `INITIALIZE_MSG` | 24 | Type@0, Length@4, RequestId@8, MajorVersion@12, MinorVersion@16, MaxTransferSize@20 |
| `INITIALIZE_CMPLT` | 52 | …, Status@12, MajorVersion@16, MinorVersion@20, DeviceFlags@24, Medium@28, **MaxPacketsPerMessage@32**, **MaxTransferSize@36**, **PacketAlignmentFactor@40**, AFListOffset@44, AFListSize@48 |
| `HALT_MSG` | 12 | …, RequestId@8 |
| `QUERY_MSG` = `SET_MSG` | 28 + payload | …, RequestId@8, Oid@12, InfoBufferLength@16, InfoBufferOffset@20, DeviceVcHandle@24 |
| `QUERY_CMPLT` | 24 + payload | …, Status@12, InfoBufferLength@16, InfoBufferOffset@20 |
| `SET_CMPLT` | 16 | …, Status@12 |
| `RESET_MSG` | 12 | …, **Reserved@8** (⚠️ not `RequestId`) |
| `RESET_CMPLT` | 16 | …, **Status@8** (⚠️ not 12), **AddressingReset@12** |
| `INDICATE_STATUS_MSG` | 20 + payload | …, Status@8, StatusBufferLength@12, StatusBufferOffset@16 |
| `KEEPALIVE_MSG` | 12 | …, RequestId@8 |
| `KEEPALIVE_CMPLT` | 16 | …, RequestId@8, Status@12 |
| `PACKET_MSG` header | 44 | Type@0, Length@4, **DataOffset@8**, DataLength@12, OOBDataOffset@16, OOBDataLength@20, NumOOBDataElements@24, PerPacketInfoOffset@28, PerPacketInfoLength@32, VcHandle@36, Reserved@40 |

Offset field values when the payload immediately follows the header:

| Message | Header Size | Offset Field Value |
|---|---:|---:|
| `PACKET_MSG` | 44 | **36** |
| `QUERY_MSG` / `SET_MSG` | 28 | **20** |
| `QUERY_CMPLT` | 24 | **16** |

---

## 3. Status Codes

Key status codes (see `StatusCode` in `protocol.h` for the complete list):

| Name | Value | Description |
|---|---|---|
| `SUCCESS` | `0x00000000` | |
| `PENDING` | `0x00000103` | |
| `MEDIA_CONNECT` | `0x4001000B` | Link up |
| `MEDIA_DISCONNECT` | `0x4001000C` | Link down |
| `LINK_SPEED_CHANGE` | `0x40010013` | |
| `FAILURE` | `0xC0000001` | |
| `NOT_SUPPORTED` | `0xC00000BB` | Normal response for optional OIDs |
| `INVALID_LENGTH` | `0xC0010014` | ⚠️ Easy to misremember (`0xC001…`, not `0xC000…`) |
| `INVALID_DATA` | `0xC0010015` | ⚠️ Easy to misremember |
| `BUFFER_TOO_SHORT` | `0xC0010016` | |
| `INVALID_OID` | `0xC0010017` | ⚠️ Easy to misremember |

> Note: Those `0xC0010014`–`0xC0010017` values live in the NDIS-specific `0xC001…` space rather than standard NTSTATUS `0xC000…`. The status code table is kept solely in `rndis/protocol.cc` as a single source of truth.

Failure predicate: `(status & 0xC0000000) == 0xC0000000`.

---

## 4. OIDs

| OID | Value | Data Format | Usage in TetherKitNext |
|---|---|---|---|
| `OID_GEN_SUPPORTED_LIST` | `0x00010101` | **Variable-length**: N × LE32 | Unused |
| `OID_GEN_MAXIMUM_FRAME_SIZE` | `0x00010106` | LE32, **excluding** Ethernet header | Validates and potentially clamps MTU |
| `OID_GEN_LINK_SPEED` | `0x00010107` | LE32, **in units of 100 bps** | Logging |
| `OID_GEN_VENDOR_ID` | `0x0001010C` | LE32, lower 24 bits = OUI | Logging |
| `OID_GEN_VENDOR_DESCRIPTION` | `0x0001010D` | **Variable-length** ASCII, **not guaranteed NUL-terminated** | Logging |
| `OID_GEN_CURRENT_PACKET_FILTER` | `0x0001010E` | LE32 bitmask (read/write) | **Must be set non-zero for data to flow** |
| `OID_GEN_MEDIA_CONNECT_STATUS` | `0x00010114` | LE32: **0 = connected**, 1 = disconnected | Initial link status |
| `OID_GEN_PHYSICAL_MEDIUM` | `0x00010202` | LE32, **optional OID** | Logging (failure is non-fatal) |
| `OID_802_3_PERMANENT_ADDRESS` | `0x01010101` | 6-byte MAC address | **Assigned to the host-side `feth` interface** |
| `OID_802_3_CURRENT_ADDRESS` | `0x01010102` | 6-byte MAC address | Logging |
| `OID_802_3_MULTICAST_LIST` | `0x01010103` | SET: 6N bytes | Unused (`ALL_MULTICAST` is enabled in the packet filter) |

⚠️ **In `OID_GEN_MEDIA_CONNECT_STATUS`, `0` means "connected"**, not disconnected.

⚠️ **Do not rely on `QUERY OID_802_3_MULTICAST_LIST`**: the Linux gadget driver returns a 4-byte `0xE0000000` value that does not conform to a 6-byte MAC array. Only use the SET direction if needed.

### Counter-Intuitive `InformationBufferLength` Rule

Discovered by reverse-engineering Microsoft ActiveSync 4.1 Windows drivers (**undocumented in the RNDIS spec**):

| OID Category | Required `InformationBufferLength` in `QUERY_MSG` | Consequence if Violated |
|---|---|---|
| Returns a **fixed-length** result | Must be ≥ expected response length, with that many zero bytes appended to the request | `RNDIS_STATUS_INVALID_LENGTH` |
| Returns a **variable-length** result | Must be **0** | Fails with status error |

`Encode(QueryRequest)` handles this automatically via `IsVariableLengthOid()`, and unit tests explicitly lock down this behavior.

---

## 5. Packet Filter Bitmask

| Name | Value |
|---|---|
| `DIRECTED` | `0x00000001` |
| `MULTICAST` | `0x00000002` |
| `ALL_MULTICAST` | `0x00000004` |
| `BROADCAST` | `0x00000008` |
| `PROMISCUOUS` | `0x00000020` |

TetherKitNext sets `DIRECTED | BROADCAST | ALL_MULTICAST | PROMISCUOUS` = **`0x0000002D`** (matching Linux `rndis_host`).

Why enable `PROMISCUOUS`: we bridge the USB device as a raw L2 pipe to `feth`, and the host may send/receive frames with arbitrary MAC addresses (e.g., when bridging or running VMs). Why enable `ALL_MULTICAST`: avoids maintaining multicast filter tables while ensuring IPv6 Neighbor Discovery always works.

---

## 6. Host-Side State Machine

The RNDIS specification defines three states on the **device side**; the host mirrors them and adds transitional states:

```
kUninitialized ──Start()/INITIALIZE──► kInitializing ──CMPLT + valid params──► kInitialized
      ▲                                     │                                       │
      │                             negotiation failed                        SET filter≠0
      │                                     │                                       ▼
      └─────────── HALT / disconnect ───────┴───────────────────────────── kDataInitialized
                                                                                    │
                                                                       SET filter=0 │ Stop()
                                                                    (returns above) ▼
                                                                                kHalting
```

Specification state semantics:

- After bus initialization, the device starts in `RNDIS-uninitialized`;
- Receiving `INITIALIZE_MSG` and replying `INITIALIZE_CMPLT(SUCCESS)` transitions to `RNDIS-initialized`;
- Receiving `SET(OID_GEN_CURRENT_PACKET_FILTER, non-zero)` transitions to `RNDIS-data-initialized`, **at which point data traffic begins flowing**;
- Receiving `SET(filter = 0)` while data-initialized transitions back to `RNDIS-initialized`;
- Receiving `HALT_MSG` or bus disconnection at any time transitions immediately to `RNDIS-uninitialized`.

### Startup Sequence (Modeled After Linux `generic_rndis_bind`)

| Step | Operation | Fatal on Failure? |
|---|---|---|
| 1 | `INITIALIZE_MSG` → `INITIALIZE_CMPLT`, negotiate parameters | ✔ |
| 2 | `QUERY OID_GEN_PHYSICAL_MEDIUM` (`in_len=4`) | ✘ (optional OID) |
| 3 | `QUERY OID_802_3_PERMANENT_ADDRESS` (`in_len=48`) | ✔ |
| 4 | `QUERY` current MAC / max frame size / link speed / media status / vendor info | ✘ |
| 5 | `SET OID_GEN_CURRENT_PACKET_FILTER = 0x2D` | ✔ |

If any fatal step fails, the state machine sends `HALT_MSG`, returns to `kUninitialized` (never getting stuck in an intermediate state), and propagates the error.

### Three Subtle Interaction Details

**① Control requests must be serialized.**
`GET_ENCAPSULATED_RESPONSE` simply dequeues the next pending response from the device and cannot select responses by `RequestId`. Only one control request may be in flight at a time.

**② The device can cut in line.**
While waiting for a `*_CMPLT` response, the device may first queue an unsolicited `INDICATE_STATUS_MSG` (e.g., media connect/disconnect) or a **device-initiated** `KEEPALIVE_MSG` — to which the host **must reply** with `KEEPALIVE_CMPLT`, or the device may declare the host dead and disconnect. Consequently, the control read loop is implemented as a message-type dispatcher rather than assuming the next message read matches the pending request.

**③ `RESET` semantics.**
`RESET` is a soft reset: the control channel remains up, but the device discards **all** pending requests and packets. A non-zero `AddressingReset` in `RESET_CMPLT` indicates that addressing state (packet filter and multicast list) was lost during reset, requiring the host to **re-send** `SET(OID_GEN_CURRENT_PACKET_FILTER)` before data will flow again. The Linux gadget driver unconditionally sets `AddressingReset = 1`.

---

## 7. USB Layer

### Control Channel

| Request | `bmRequestType` | `bRequest` | `wValue` | `wIndex` |
|---|---|---|---|---|
| `SEND_ENCAPSULATED_COMMAND` | `0x21` (OUT\|Class\|Interface) | `0x00` | 0 | Communication interface number |
| `GET_ENCAPSULATED_RESPONSE` | `0xA1` (IN\|Class\|Interface) | `0x01` | 0 | Communication interface number |

⚠️ **When the device has no response ready yet, the RNDIS specification requires it to return a single `0x00` byte rather than stalling the control endpoint.** Therefore, any response shorter than 8 bytes (the minimum RNDIS header size) must be treated as "not ready yet, retry later" and **never as a fatal error**.

### Interrupt IN Notification

8 bytes = two `LE32` words: `Notification`@0 (`0x00000001` = `RESPONSE_AVAILABLE`), `Reserved`@4.

⚠️ This is **not** a standard CDC `usb_cdc_notification` header; it is an RNDIS-specific format.

Devices exhibit three distinct behaviors in the wild, and our implementation supports all of them:

1. Spec-compliant: device sends an Interrupt IN notification before each control response;
2. Linux approach: ignore the interrupt endpoint entirely and poll `GET_ENCAPSULATED_RESPONSE` up to 10 times at 40 ms intervals;
3. Quirky devices that **refuse to answer on the control endpoint unless the interrupt endpoint has been read at least once**.

TetherKitNext's strategy: if an Interrupt IN endpoint exists, wait briefly for a notification (ignoring timeouts), and then poll `GET_ENCAPSULATED_RESPONSE` **regardless**.

### Interface Signatures (Four Known Variants)

| Variant | Communication Interface | Data Interface |
|---|---|---|
| Standard Microsoft RNDIS | `0x02 / 0x02 / 0xFF` | `0x0A / 0x00 / 0x00` |
| ActiveSync (Windows Mobile / Phone) | `0xEF / 0x01 / 0x01` | Same as above |
| Wireless RNDIS (Android USB tethering, WWAN modules) | `0xE0 / 0x01 / 0x03` | Same as above |
| Novatel/Verizon USB730L variant | `0xEF / 0x04 / 0x01` | Same as above |

**Filtering out false positives**: an interface with `class == 0x02` and a **non-zero** CDC ACM `bmCapabilities` descriptor is a genuine `cdc-acm` modem, not RNDIS. ⚠️ **This check must only be applied when `class == 0x02`** — wireless RNDIS interfaces (`class == 0xE0`) repurpose the `bmCapabilities` byte for their own flags.

---

## 8. Multi-Packet Aggregation and ZLP Avoidance

### Aggregation Rules

- **Device → Host**: each `PACKET_MSG` starts at an 8-byte-aligned offset from the beginning of the multi-packet transfer;
- **Host → Device**: must align each `PACKET_MSG` to `1 << PacketAlignmentFactor` bytes as reported in `INITIALIZE_CMPLT` (**valid maximum factor is 7**, i.e., 128-byte alignment);
- **`MessageLength` includes inter-packet alignment padding**: every `PACKET_MSG` except the last includes its trailing alignment padding inside `MessageLength`, whereas the last `PACKET_MSG` does **not** include trailing padding;
- Total bytes per transfer ≤ peer's `MaxTransferSize`, and total packets ≤ `MaxPacketsPerMessage`;
- Iteration rule: `offset += MessageLength`.

Our encoder folds padding into the **previous** message only when appending a subsequent message, ensuring that the final message's `MessageLength` is always exact (`44 + frame_length`).

### Zero-Length Packet (ZLP) Avoidance

RNDIS explicitly forbids the host from sending zero-length USB packets (`rndis_host.c` header comment: `"DATA -- host must not write zlps"`). However, when a USB transfer length is an exact multiple of the endpoint's `wMaxPacketSize`, the USB host controller expects a short packet to terminate the transfer.

Following Linux `usbnet`, TetherKitNext **appends a single `0x00` byte** whenever the transfer length is a multiple of `wMaxPacketSize`, turning it into a short packet without sending a ZLP. That extra byte lies outside `MessageLength` and is ignored by the device.

⚠️ Conversely, **the host RX parser must tolerate `actual_length > sum(MessageLength)`**. Trailing bytes shorter than a 44-byte `PACKET_MSG` header are treated as padding and silently ignored.

⚠️ `LIBUSB_TRANSFER_ADD_ZERO_PACKET` is **not supported on macOS** (returns `LIBUSB_ERROR_NOT_SUPPORTED`) and is never used.

---

## 9. `MaxTransferSize` — The Primary Lever for RX Throughput

The `MaxTransferSize` advertised by the host in `INITIALIZE_MSG` is the **only mechanism that enables the device to pack multiple `PACKET_MSG` frames into a single bulk IN transfer**. The larger the value, the more frames the device can aggregate, reducing USB and syscall overhead per frame.

Linux uses a conservative single-frame formula:

```
hard_header_len = ETH_HLEN(14) + sizeof(rndis_data_hdr)(44) = 58
hard_mtu        = MTU + 58                    # MTU=1500 → 1558
rx_urb_size     = (hard_mtu + maxpacket + 1) & ~(maxpacket - 1)
                # High-speed (512) → 2048; Full-speed (64) → 1600
```

TetherKitNext advertises **16 KiB** by default (configurable via `--max-transfer-kb`), providing ample headroom for multi-packet aggregation while staying within the `0x4000` USB 1.1 specification limit. Bulk IN receive buffers scale up automatically to be at least as large as the advertised `MaxTransferSize`.

---

## 10. Known Device Quirks

| Quirk | Symptom | Handling in TetherKitNext |
|---|---|---|
| **Broken Android CDC Union descriptor** | CDC Union descriptor references non-existent interface numbers or is missing entirely | Falls back to hardcoding interface 0 = control, interface 1 = data (matching Linux `android_rndis_quirk`) and logs the fallback |
| **`MaxPacketsPerMessage = 0`** | Some devices report 0 | Clamped to 1 |
| **Qualcomm uplink aggregation patch** | `MaxPacketsPerMessage` defaults to 3 | Honored directly; TX aggregates up to 3 packets per transfer |
| **Out-of-range `PacketAlignmentFactor`** | Device reports factor > 7 | Clamped to 7 (preventing undefined shift `1u << 31`) |
| **Undersized `MaxTransferSize`** | HTC Diamond reports 1536 < `hard_mtu` (1558) | Clamps host MTU down to `1536 - 58 = 1478` |
| **Oversized `MaxTransferSize`** | WinCE / Windows Mobile devices report 8 KB or 16 KB | Clamped to the host's own buffer limit |
| **Mainline Linux gadget defaults** | `MaxPacketsPerTransfer=1`, `MaxTransferSize=1580`, `PacketAlignmentFactor=0`, `DeviceFlags=CONNECTIONLESS`, `Medium=802_3` | Used as the baseline profile for `MakeWellBehavedDevice` in `tests/mock_control_channel.h` |

---

## 11. Timeout Constants

| Constant | Spec Value | TetherKitNext Value | Rationale |
|---|---|---|---|
| `ControlTimeoutPeriod` | 10 s | **5 s** | Matches Linux (shortened for ActiveSync compatibility; a link taking >5 s to answer a control message is unusable anyway) |
| `KeepAliveTimeoutPeriod` | 5 s | 5 s | Sent only when 5 s have elapsed since **any** message was last received from the device |
| Control buffer size | ≥ 1024 B | **1536 B** | Windows uses 1025 B (copied by Linux); 1536 B provides clean alignment and extra headroom |

⚠️ On Apple Silicon, libusb **control transfers** are ~10× slower than on x86_64 ([libusb issue #1288](https://github.com/libusb/libusb/issues/1288)), so keepalive intervals should not be made aggressively short.
