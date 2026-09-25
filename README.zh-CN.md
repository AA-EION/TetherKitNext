<p align="center">
  <img src="docs/assets/icon.png" width="128" alt="TetherKit 图标">
</p>

# TetherKit

[English](README.md) | **简体中文**

> **TetherKitNext** 是 [XiaoMiku01/TetherKit](https://github.com/XiaoMiku01/TetherKit) 的维护分支：
> 签名并公证的通用 DMG（Apple Silicon + Intel）、命令行工具内置于 App、重新设计的 Liquid Glass 界面、
> 安全审计（[docs/SECURITY-AUDIT.md](docs/SECURITY-AUDIT.md)），并修复了上游 issue #1、#2、#3、#5。

**HoRNDIS 在新系统上用不了的替代方案 —— 不装内核扩展，让 Android USB 网络共享在 macOS 上重新可用。**

TetherKit 是一个 **macOS 用户态 RNDIS 驱动**：把 RNDIS 设备（Android 手机的 USB 网络共享、
Windows Phone、嵌入式 Linux gadget 等）变成一张系统可见的网卡，**不需要 kext、不需要
DriverKit、不用关 SIP、不用开发者账号**。Apple Silicon 与 Intel 走同一条代码路径。
自带图形界面，「选设备 → 连接 → 配 IP」三步点击；也提供命令行工具 `tetherkit-cli`。

![TetherKit 主界面](docs/assets/screenshot-main-zh.jpg)

- **USB 侧**：[libusb](https://libusb.info/) 异步批量传输，完整实现 RNDIS 主机侧状态机。
- **网卡侧**：macOS 的 `feth`（`if_fake`）虚拟网卡对 + BPF，直接读写原始以太帧。
- **无内核代码**：纯用户态 C++23，不需要 kext、不需要 DriverKit、不需要关 SIP。

> ⚠️ **状态**：全部模块已实现，210 个测试用例（10408 条断言）在普通构建与
> ThreadSanitizer 构建下均通过。端到端已用 Android 手机压测：**RX 约 326 Mbps /
> TX 约 240~300 Mbps，双向零重传**（USB 2.0 高速，理论上限 426 Mbps）。
> 测量方法与完整结果见 [docs/BENCHMARKS.md](docs/BENCHMARKS.md)，
> 验证清单见 [AGENTS.md](AGENTS.md) 第 6 节。

---

## 从 HoRNDIS 过来的？

如果你是因为 **HoRNDIS 升级完 macOS 就用不了了**，或者在 Apple Silicon 上它一直显示
「已阻止的软件」、根本找不到那个「允许」按钮 —— TetherKit 就是为这种情况写的。

[HoRNDIS](https://github.com/jwise/HoRNDIS) 好用了很多年，它本身没有问题。问题在于它是一个
**内核扩展**，而这正是 macOS 一直在收紧的地方：

| | HoRNDIS | TetherKit |
|---|---|---|
| 是什么 | 内核扩展（kext） | 普通的用户态程序 |
| Apple Silicon | 要先进恢复模式把安全策略降到「降低安全性」并重启，第三方 kext 才**有可能**加载 | 什么都不用改 —— 在系统眼里它根本不是驱动 |
| SIP | 通常需要关掉或削弱 | 不动 |
| 升级 macOS 之后 | kext 可能直接不加载，往往要重新批准或重新编译 | 就是个 App，没有什么要重新批准的 |
| 出问题的最坏情况 | 内核崩溃（kernel panic） | 进程退出 |
| 上游状态 | 最新发布 9.2（2024 年 8 月）；[「MacOS Sonoma not supported」](https://github.com/jwise/HoRNDIS/issues/169) 仍未关闭 | 在积极开发 |

TetherKit 是从另一头解决同一个问题：不去教内核说 RNDIS，而是**在用户态**通过 libusb 把
RNDIS 说完，再借 `feth` 虚拟网卡对把以太帧交给系统。内核自始至终没有加载我们的任何代码。

**代价说在前面**：网卡由一个需要你启动的 App 创建（连接本身不受界面退出影响 —— 它跑在一个
很小的特权后台组件里）；吞吐受限于用户态拷贝而不是 USB 链路。不过实测下来这并没有成为瓶颈，
数据见上面。

---

## 为什么需要它

macOS 内核**没有** RNDIS 驱动。插上开了 USB 网络共享的 Android 手机，系统不会出现新网卡。
既有方案要么写 kext（需要关 SIP 或降低 Apple Silicon 的安全策略、签名门槛高、系统更新易
失效），要么走 Network Extension（需要开发者账号 + 系统扩展审批）。TetherKit 选择第三条
路：**在用户态同时扮演 USB 主机和网卡驱动**。

---

## 安装

**兼容性**：macOS 14 Sonoma、15 Sequoia、26 Tahoe，Apple Silicon 与 Intel 通用（一个通用二进制 App）。
内置的命令行工具支持 macOS 13.3 Ventura 及以上。无需关闭任何安全设置 —— SIP 保持开启，
Apple Silicon 的安全策略保持「完整安全性」。

1. 从 [Releases](https://github.com/AA-EION/TetherKitNext/releases) 下载
   **TetherKit-x.y.z.dmg**，打开后把 **TetherKit** 拖到 **应用程序**。
2. 从“应用程序”打开 TetherKit，点击 **启用后台组件**。macOS 会请你在
   *系统设置 › 通用 › 登录项与扩展* 中允许一次；允许后 App 会自动继续。
3. 可选 —— 命令行：*设置 › 命令行工具 › 安装命令* 会把 `tetherkit-cli` 链接到
   `/usr/local/bin`，之后在任何终端里都能用 `tetherkit-cli --list` 和 `sudo tetherkit-cli`。

DMG 使用 Developer ID 签名并经过 Apple 公证，打开时不会有 Gatekeeper 警告。所有东西都在 App 里：
图形界面、`tetherkit-cli`、后台组件，以及自带的 libusb —— 不需要 Homebrew。

**更新**：下载新的 DMG，替换“应用程序”里的 App 即可。后台组件直接从 App 内运行，会随之更新
（如果正在运行的是旧版本，设置页会提供一键重启）。

**卸载**：*设置 › 后台组件 › 停用…*（同时移除 `tetherkit-cli` 链接），然后把 App 移到废纸篓。

> **从上游 TetherKit（Homebrew 版）升级？** 新的后台组件首次启动时会自动移除旧的
> `com.tetherkit.helper` LaunchDaemon 及其文件，之后可以执行 `brew uninstall tetherkit tetherkit-cli`。

---

## 图形界面

TetherKit.app（SwiftUI，在 macOS 26 上采用 Liquid Glass 设计）按任务分布在侧边栏：

| 页面 | 用途 |
|---|---|
| 概览 | 连接状态、实时吞吐、网卡地址一览 |
| 设备 | 选择手机（或其他 RNDIS 设备），MTU 与 MAC 选项 |
| 网络 | 自动（DHCP）或静态 IP、DNS、默认路由 |
| 日志 | 驱动实时日志，可筛选、可复制 |
| 设置 | 后台组件、命令行工具、语言、菜单栏、登录时打开、更新 |

**连接 / 断开** 在每个页面的工具栏上（⌘↩），菜单栏面板里也有。

App 以**普通用户**身份运行。需要 root 的工作（创建虚拟网卡、打开 BPF、配置 IP）交给
`tetherkit-helper` —— 一个通过 `SMAppService` 注册、直接从已签名 App 包内运行的后台组件。
正式构建只允许签名一致的 App 与它通信（XPC 代码签名要求），并且每次特权调用还需附带管理员授权。
详见 [docs/SECURITY-AUDIT.md](docs/SECURITY-AUDIT.md)。

上网方式：

| 方式 | 说明 |
|---|---|
| 自动（DHCP） | 把网卡注册为真正的 macOS 网络服务，DNS、路由以及 VPN NetworkExtension（FortiClient 等）都能正常工作。推荐 |
| 静态 IP | 自行填写地址、掩码、网关与 DNS，输入时即时校验 |

**菜单栏模式**：关闭主窗口后程序坞图标隐藏，TetherKit 留在菜单栏，可选显示定宽的实时上下行速率
（在设置中开关）。连接由后台组件维持，关闭或退出界面都不会断开。

**更新**：*TetherKit › 检查更新…* 或设置中的每日自动检查，只读取本仓库公开的 GitHub Releases
并打开发布页 —— 不会自行下载或安装任何东西。

**语言**：跟随系统 / 中文 / English，可在 App 菜单、菜单栏面板或设置中即时切换。

设计说明与取舍见 [docs/GUI-ARCHITECTURE.md](docs/GUI-ARCHITECTURE.md)。

---

## 命令行工具

图形界面之外还有命令行工具 `tetherkit-cli`（安装见上文）：

```bash
# 先看看设备有没有被识别（**不需要 root**）
tetherkit-cli --list

# 启动驱动
sudo tetherkit-cli

# 另开一个终端，给新出现的网卡配 IP（RNDIS 设备通常自带 DHCP 服务器）
sudo ipconfig set feth0 DHCP
ipconfig getifaddr feth0
```

> 上面的命令都按已安装（`tetherkit-cli` 在 PATH 里）来写。从源码构建的话
> 换成 `./build/bin/tetherkit-cli` 即可。

启动成功后程序会打印它创建的网卡名与后续命令。按 `Ctrl-C` 优雅退出
（会先让设备退出 RNDIS，再销毁网卡）。

常用选项（完整列表见 `--help`）：

| 选项 | 说明 |
|---|---|
| `--list` | 列出识别到的 RNDIS 设备后退出，不需要 root |
| `--vid` / `--pid` | 指定设备（十六进制），用于多设备场景 |
| `--stats 1000` | 每秒打印一行吞吐/丢包统计 |
| `--log debug` | 打开协议交互细节日志 |
| `--max-transfer-kb` | 吞吐的主要调优旋钮，见 [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| `--lang zh\|en\|auto` | 界面语言，默认 `auto`（见下） |

**语言**：默认按 `TETHERKIT_LANG` → `LC_ALL` → `LC_MESSAGES` → `LANG` 依次推断，
取第一个非空值，以 `zh` 开头算中文、其余算英文。

```bash
tetherkit-cli --lang en --help     # 显式指定
TETHERKIT_LANG=en tetherkit-cli --list
```

> ⚠️ `sudo` 是否把 `LANG` 透传给命令取决于 sudoers 的 `env_keep`，因此
> `sudo tetherkit-cli` 未必跟随你的终端语言 —— 那时显式写 `--lang`。

### 为什么需要 root

| 操作 | 需要 root？ | 原因 |
|---|---|---|
| 创建 / 销毁 `feth` | ✔ | 内核对 `SIOCIFCREATE2` / `SIOCSDRVSPEC` 有 `proc_suser` 检查 |
| 打开 `/dev/bpf*` | ✔ | 节点是 `0600 root:wheel`，且 macOS **没有** FreeBSD 的 `access_bpf` 组 |
| libusb 声明 RNDIS 接口 | ✘ | macOS 内核**没有** RNDIS 驱动，接口本来就没人占。非沙箱命令行程序不需要 root、也不需要 entitlement |

也就是说 root 是网卡侧的要求，不是 USB 侧的。`--list` 因此不需要 root。

---

## 故障排查

| 现象 | 原因与对策 |
|---|---|
| `--list` 找不到设备 | ① 换一根**数据线**（很多线只供电）；② 在设备上开启「USB 网络共享 / USB tethering」；③ 解锁手机并信任本机。用 `system_profiler SPUSBDataType` 确认系统是否看到该设备 |
| `声明 RNDIS 数据接口失败 [libusb: LIBUSB_ERROR_ACCESS]` | 接口被别的东西占了。检查是否装过 HoRNDIS 之类的第三方 kext，或有别的用户态程序在用该设备 |
| `创建 feth 虚拟网卡需要 root` | 用 `sudo` 运行 |
| `sysctl net.link.fake.hwcsum 当前是 1，要求 0` | 这批开关在 feth **创建时被快照**，创建后改无效。按提示先 `sudo sysctl -w net.link.fake.hwcsum=0` 再启动 |
| `ipconfig set feth0 DHCP` 拿不到地址 | 确认设备侧的网络共享真的开着；用 `--stats 1000` 看 TX 有没有帧发出去、RX 有没有回包 |
| 网卡配好了但上不了网 | 默认路由还指向原来的网卡。`sudo route -n change default $(ipconfig getoption feth0 router)`，注意这会顶掉现有默认路由 |
| 吞吐远低于预期 | 看启动日志里的「设备聚合上限 N 包」与「链路批量写」两项，再对照 [docs/PERFORMANCE.md](docs/PERFORMANCE.md) 的排查表 |
| 重启后网卡不见了 | `ipconfig set` 建立的是**临时**服务，只存活到下一次网络配置变更，且不出现在系统设置里。这是 macOS 的限制，不是 bug |

### 常见疑问

**Android 的 USB 网络共享在 macOS 上到底能不能用？**
开箱即用是不能的 —— macOS 不带 RNDIS 驱动，插上开了 USB 网络共享的手机，系统里不会多出
任何网卡。TetherKit 补的就是这一块。

**要关 SIP 吗？Apple Silicon 上要降安全策略吗？**
都不用。那些是**内核扩展**的要求。TetherKit 是普通程序，内核不会加载它的任何代码；需要
特权的只是一个后台小组件，安装时输一次管理员密码，之后不再打扰。

**HoRNDIS 显示「已阻止的软件」、没有「允许」按钮，能修吗？**
那是 macOS 在拒绝加载第三方 kext，不是 kext 自己能解决的。Apple Silicon 上想加载它，必须
先进恢复模式降低安全策略。TetherKit 从根上绕开了这一整类问题。

**M1 / M2 / M3 / M4 能用吗？**
能。Apple Silicon 与 Intel 走同一条代码路径，除了编译目标之外没有任何架构相关的东西。

**我的设备支持吗？**
只要它暴露 RNDIS 接口就行：多数开了 USB 网络共享的 Android 手机、Windows Phone，以及
Linux 的 `g_ether` / `u_ether` USB gadget（树莓派 Zero、BeagleBone 等）。
跑 `tetherkit-cli --list` 就能确认，**不需要 root**。

**iPhone 的 USB 共享需要它吗？**
不需要。macOS 原生支持 iPhone 的 USB 网络共享，TetherKit 面向的是它不覆盖的那些设备。

---

## 架构

```
        ┌──────────────────────────── macOS 内核 ────────────────────────────┐
        │                                                                    │
        │   IP 栈 / 路由 / DHCP 客户端                                        │
        │        │                                                           │
        │        ▼                                                           │
        │   ┌─────────┐   if_fake peer 对   ┌─────────┐                       │
        │   │  feth0  │ ◄─────────────────► │  feth1  │                       │
        │   │(系统侧) │                     │(驱动侧) │                       │
        │   └─────────┘                     └────┬────┘                      │
        │    配 IP/路由                          │ BPF                       │
        └────────────────────────────────────────┼───────────────────────────┘
                                                 │ read()/write() 原始以太帧
        ┌────────────────────────────────────────┼───────────────────────────┐
        │                     TetherKit（用户态） │                           │
        │                                        ▼                           │
        │   ┌──────────────────── 数据路径桥接 ─────────────────────┐         │
        │   │  TX: BPF 批量读 → 聚合多帧 → RNDIS_PACKET_MSG → bulk OUT│        │
        │   │  RX: bulk IN → 拆 RNDIS_PACKET_MSG → SPSC 队列 → BPF 写 │        │
        │   └───────────────────────────┬───────────────────────────┘         │
        │                               │                                     │
        │   ┌──────────── RNDIS 状态机 ──┴──────────────┐                      │
        │   │ INITIALIZE / QUERY / SET / KEEPALIVE /   │                      │
        │   │ RESET / INDICATE_STATUS / HALT           │                      │
        │   └───────────────────────────┬──────────────┘                      │
        │                               │                                     │
        │   ┌────────── libusb ─────────┴──────────────┐                      │
        │   │ 控制通道：SEND_ENCAPSULATED_COMMAND /     │                      │
        │   │           GET_ENCAPSULATED_RESPONSE      │                      │
        │   │ 通知通道：中断 IN（RESPONSE_AVAILABLE）    │                      │
        │   │ 数据通道：bulk IN / bulk OUT（异步池化）   │                      │
        │   └───────────────────────────┬──────────────┘                      │
        └───────────────────────────────┼─────────────────────────────────────┘
                                        │ USB
                                   ┌────┴─────┐
                                   │ RNDIS 设备 │
                                   └──────────┘
```

---

## 从源码构建

要求：macOS 14+、**Xcode 26**（Swift 6.2、macOS 26 SDK）、CMake ≥ 3.24。
libusb 由下面的脚本从固定版本、校验哈希的发布包构建；本地快速构建 C++ 部分也可以用 Homebrew 的 `libusb`。

```bash
./scripts/build-libusb.sh "$PWD/build/libusb-universal"
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLibUSB_ROOT="$PWD/build/libusb-universal"
cmake --build build -j
```

产物：`build/bin/tetherkit-cli`。

### 构建 App 与 DMG

完整的发布流程 —— 通用 libusb、通用 C++ 与测试、App 包、DMG —— 是一个脚本，与 CI 执行的完全相同：

```bash
./scripts/build-release.sh        # → dist/TetherKit.app、dist/TetherKit-<版本>.dmg
```

签名与公证由环境变量控制：

| 变量 | 用途 |
|---|---|
| `TETHERKIT_SIGN_IDENTITY` | 例如 `Developer ID Application: Jane Doe (ABCDE12345)`。不设则 ad-hoc 签名（仅供本地测试） |
| `NOTARY_KEY_PATH`、`NOTARY_KEY_ID`、`NOTARY_ISSUER_ID` | `notarytool` 使用的 App Store Connect API 密钥（或 `NOTARY_APPLE_ID` / `NOTARY_PASSWORD` / `NOTARY_TEAM_ID`） |

要在本机测试后台组件，请用 Apple Development 或 Developer ID 证书签名 —— macOS 拒绝注册 ad-hoc 签名的守护进程。

在 GitHub Actions 中添加仓库密钥 `MACOS_CERTIFICATE_P12`（导出的证书 + 私钥的 base64）、
`MACOS_CERTIFICATE_PASSWORD`、`NOTARY_KEY_P8`（.p8 的 base64）、`NOTARY_KEY_ID`、`NOTARY_ISSUER_ID`。
之后推送 `v*` 标签即会发布签名并公证的 DMG（[release.yml](.github/workflows/release.yml)）。

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `TETHERKIT_BUILD_TESTS` | `ON` | 构建单元测试 |
| `TETHERKIT_BUILD_BENCHMARKS` | `ON` | 构建性能基准 |
| `TETHERKIT_WARNINGS_AS_ERRORS` | `OFF` | 警告当错误 |
| `TETHERKIT_NATIVE_ARCH` | `OFF` | `-mcpu=native`，产物不可移植 |
| `TETHERKIT_ENABLE_LTO` | `OFF` | 链接时优化 |
| `TETHERKIT_ENABLE_ASAN` | `OFF` | AddressSanitizer |
| `TETHERKIT_ENABLE_UBSAN` | `OFF` | UndefinedBehaviorSanitizer |
| `TETHERKIT_ENABLE_TSAN` | `OFF` | ThreadSanitizer（**验证无锁数据结构必备**） |

### 测试

```bash
ctest --test-dir build --output-on-failure
```

无锁队列与多线程数据路径的正确性用 ThreadSanitizer 单独验证：

```bash
cmake -S . -B build-tsan -DTETHERKIT_ENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

### 性能基准

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
./build-rel/bin/tetherkit_bench
```

汇总结果见 [docs/BENCHMARKS.md](docs/BENCHMARKS.md)。

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | **总体设计**：为什么选 feth+BPF、模块划分、并发模型、拆除顺序、私有 ABI 的风险评估 |
| [docs/RNDIS-PROTOCOL.md](docs/RNDIS-PROTOCOL.md) | **协议参考**：字段偏移、状态码、OID、状态机、设备 quirk。含三条最容易搞错的规则 |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | **调优指南**：旋钮、怎么判断瓶颈、已知限制 |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | **基准结果**（自动生成）+ 测量方法与已知局限 |
| [docs/GUI-ARCHITECTURE.md](docs/GUI-ARCHITECTURE.md) | **图形界面**：进程与信任模型、数据流、实现约束、已实现/未实现清单 |
| [docs/GUI-SPIKE.md](docs/GUI-SPIKE.md) | **可行性验证**：为什么这么做特权提升，以及被排除的三条路线 |
| [AGENTS.md](AGENTS.md) | 实现备忘：已实测确认的环境事实、进度、**踩过的坑**、待验证清单 |

---

## 许可

见 [LICENSE](LICENSE)。
