import Foundation

// 图形界面与 helper 的全部面向用户文案。
//
// 加一条：在 `L10nKey` 里加 case，再到下面的 switch 里补中英两版。switch 是
// 穷尽的，漏了直接编译不过。
//
// ★ 占位符的规矩 ★
//
//   走 `String(format:)`，printf 风格：`%@` 字符串、`%ld` 整数、`%.1f` 浮点。
//   **两种语言的占位符必须一一对应**（个数、顺序、类型）。译文需要换语序时，
//   两边都改成带位置的形式（`%1$@`、`%2$ld`）—— 只有一边带位置也是合法的
//   printf，但那样两个串的参数含义就悄悄错位了。
//
//   调用点传参前统一转成 `Int` / `Double` / `String`：`UInt32` 之类的整型直接
//   喂给 `%ld` 在 32/64 位上的行为不一致，转一次最省心。
//
//   这两条由 TetherKitIPCTests 的 LocalizationTests 逐条核对。

/// 一条文案的标识。
public enum L10nKey: String, CaseIterable, Sendable {

    // MARK: - 通用

    case ok
    case cancel
    case copy
    case copied
    case add
    case apply
    case connect
    case disconnect
    case uninstall
    case ipAddress
    case netmask
    case router
    case notConfigured
    case listSeparator

    // MARK: - 语言设置

    case languageLabel
    case languageSystem
    case languageChinese
    case languageEnglish
    case languageMenuTitle

    // MARK: - 数据模型（Models.swift）

    case usbDeviceFallbackName
    case ipModeDhcp
    case ipModeManual
    case ipModeNone

    // MARK: - 授权（Authorization.swift）

    case authorizationCancelled
    case authorizationDenied
    case authorizationSessionFailed
    case authorizationBlobMalformed
    case authorizationRestoreFailed
    case authorizationRightMissing

    // MARK: - 库错误（TetherKitError.swift）

    case libraryGenericFailure

    // MARK: - 会话事件（TetherKitSession.swift）

    case eventLinkUp
    case eventLinkDown
    case eventDeviceResetReplayed
    case eventDeviceReset
    case eventNegotiated

    // MARK: - 网络参数校验（NetworkConfigurator.swift）

    case invalidIPAddress
    case invalidNetmask
    case invalidRouter
    case routerRequiredForDefaultRoute
    case invalidDNSServer

    // MARK: - helper 服务（HelperService.swift / main.swift / InstallerMode.swift）

    case helperSessionAlreadyRunning
    case helperSessionStopped
    case helperNetworkApplied
    case helperRequestDecodeFailed
    case helperReplyEncodeFailed
    case helperOrphansCleaned
    case helperOrphanCleanupFailed
    case helperSigtermReceived
    case helperReady

    // MARK: - helper 客户端（HelperClient.swift / HelperInstaller.swift）

    case helperConnectFailed
    case helperReplyUnparsable

    // MARK: - 检查更新（UpdateChecker.swift）

    case updateNoReleases
    case updateHTTPStatus
    case updateBadResponse


    // MARK: - Distribution & system integration

    case confirmHelperUpdateTitle
    case confirmUninstallTitle
    case helperUpdateWhileRunningWarning
    case helperVersionMismatchTooltip
    case installHelperButton
    case installHelperDetail
    case installingProgress
    case needInstallBody
    case needInstallTitle
    case uninstallExplanation
    case uninstallHelperMenuItem
    case updateAvailable
    case updateHelperButton
    case updateHelperDetail
    case cliLinkNotInBundle
    case cliLinkToolMissing
    case cliLinkDirectoryUnsafe
    case cliLinkOccupied
    case cliLinkSystemError
    case moveToApplicationsRequired
    case helperRegisterFailed
    case authPromptCommandLineTool
    case helperApprovalTitle
    case helperApprovalBody
    case openLoginItemsSettings
    case commandLineToolTitle
    case commandLineToolInstalled
    case commandLineToolNotInstalled
    case commandLineToolOccupied
    case installCommandLineTool
    case removeCommandLineTool
    case paneOverview
    case paneDevice
    case paneNetwork
    case paneActivity
    case paneSettings
    case configureNetwork
    case setupTitle
    case setupSubtitle
    case setupStepMoveTitle
    case showInFinder
    case waitingForApproval
    case settingsBackgroundSection
    case settingsBackgroundStatus
    case backgroundRunning
    case settingsGeneralSection
    case menuBarShowSpeed
    case launchAtLogin
    case launchAtLoginFailed
    case settingsUpdatesSection
    case autoCheckUpdates
    case checkNow
    case currentVersion
    case settingsAboutSection
    case projectWebsite
    case showLicenses
    case autoConfigureNetwork
    case autoConfigureNetworkHelp

    // MARK: - 连接状态（DesignSystem.swift）

    case statusDisconnected
    case statusConnecting
    case statusConnected
    case statusReadyLinkDown
    case statusDisconnecting
    case statusStopped
    case statusFailed

    // MARK: - 应用模型（AppModel.swift）

    case authPromptSession
    case interfaceNotReadyYet

    // MARK: - 主界面（ContentView.swift）

    case menuCheckForUpdates
    case alertOperationFailed
    case updateCheckTitle
    case openReleasePage
    case updateUpToDate
    case updateCheckFailed
    case updateDevBuild
    case uninstallWhileRunningWarning
    case helperComponentVersion
    case helperVersionMismatch
    case helperUpdateBadge
    case checkingHelper
    case showConnectFailureDetail
    case helperNeedsUpdateTitle
    case helperNeedsUpdateBody
    case copiedToClipboard
    case copyCommand
    case sysctlNeedsFixTitle
    case sysctlNeedsFixBody

    // MARK: - 设备卡（DeviceCard.swift）

    case usbDeviceSectionTitle
    case cannotRescanWhileRunning
    case rescanUSBDevices
    case noDeviceDetected
    case deviceChecklistCable
    case deviceChecklistTethering
    case deviceChecklistUnlocked
    case mtuBytes
    case mtuHelp
    case mtuHelpWithLimit
    case mtuTooltip
    case adoptDeviceMAC
    case adoptDeviceMACHelp
    case adoptDeviceMACTooltip
    case compatibilityMode
    case compatibilityModeTooltip

    // MARK: - 日志卡（LogCard.swift）

    case logSectionTitle
    case logDroppedNotice
    case logLevelLabel
    case logLevelAll
    case logLevelDebug
    case logLevelInfo
    case logLevelWarning
    case logLevelError
    case logAutoScroll
    case logCopyAll
    case logClear
    case logRepeatSuffix
    case logRepeatTooltip

    // MARK: - 菜单栏面板（MenuBarPanel.swift）

    case downstreamShort
    case upstreamShort
    case noIPConfigured
    case menuBarNoDevice
    case menuBarReady
    case openMainWindow
    case quit
    case quitTooltip

    // MARK: - 网络卡（NetworkCard.swift）

    case dnsEffectivenessTooltip
    case ipModeLabel
    case connectBeforeConfiguring
    case dhcpHelp
    case dhcpTooltip
    case routerOptional
    case deleteThisEntry
    case addDNSServer
    case setDefaultRoute
    case setDefaultRouteHelp
    case setDefaultRouteTooltip
    case clearConfiguration
    case clearConfigurationTooltip
    case currentlyEffective
    case primaryDefaultRoute
    case notEffective
    case noAddressYet

    // MARK: - 状态卡（StatusHeroCard.swift）

    case linkUp
    case linkDown
    case transferPaused
    case pendingDevice
    case noRNDISDeviceDetected
    case interfaceLabel
    case interfaceCreating
    case connectDisabledHint

    // MARK: - 吞吐卡（ThroughputCard.swift）

    case throughputSectionTitle
    case throughputLive
    case throughputDownstream
    case throughputUpstream
    case throughputDownstreamFPS
    case throughputUpstreamFPS
    case chartTime
    case chartRate
    case chartDirection
    case throughputCollecting
    case throughputPlaceholder
    case totalDownstream
    case totalUpstream
    case downstreamFrames
    case upstreamFrames
    case downstreamDropped
    case upstreamDropped
    case kernelDrops
    case transmitBackpressure
}

extension L10nKey {

    /// 一条文案的中英两版。**两边的占位符必须一一对应。**
    // swiftlint:disable:next function_body_length cyclomatic_complexity
    public var localizations: (chinese: String, english: String) {
        switch self {

        // MARK: 通用

        case .ok: return ("好", "OK")
        case .cancel: return ("取消", "Cancel")
        case .copy: return ("复制", "Copy")
        case .copied: return ("已复制", "Copied")
        case .add: return ("添加", "Add")
        case .apply: return ("应用", "Apply")
        case .connect: return ("连接", "Connect")
        case .disconnect: return ("断开", "Disconnect")
        case .uninstall: return ("卸载", "Uninstall")
        case .ipAddress: return ("IP 地址", "IP address")
        case .netmask: return ("子网掩码", "Subnet mask")
        case .router: return ("网关", "Router")
        case .notConfigured: return ("未配置", "Not configured")
        case .listSeparator: return ("、", ", ")

        // MARK: 语言设置

        case .languageLabel: return ("界面语言", "Language")
        case .languageSystem: return ("跟随系统", "Follow system")
        case .languageChinese: return ("中文", "中文")
        case .languageEnglish: return ("English", "English")
        case .languageMenuTitle: return ("语言", "Language")

        // MARK: 数据模型

        case .usbDeviceFallbackName: return ("USB 设备 %04lx:%04lx", "USB device %04lx:%04lx")
        case .ipModeDhcp: return ("自动（DHCP）", "Automatic (DHCP)")
        case .ipModeManual: return ("静态 IP", "Static IP")
        case .ipModeNone: return ("不配置", "None")

        // MARK: 授权

        case .authorizationCancelled: return ("已取消授权", "Authorization cancelled")
        case .authorizationDenied: return ("授权未通过（%ld）", "Authorization was denied (%ld)")
        case .authorizationSessionFailed:
            return ("无法创建授权会话（%ld）", "Cannot create an authorization session (%ld)")
        case .authorizationBlobMalformed:
            return ("授权凭据格式不正确", "The authorization credential is malformed")
        case .authorizationRestoreFailed:
            return ("无法还原授权凭据（%ld）", "Cannot restore the authorization credential (%ld)")
        case .authorizationRightMissing:
            return ("调用方没有执行该操作所需的授权（%ld）",
                    "The caller lacks the authorization this operation requires (%ld)")

        // MARK: 库错误

        case .libraryGenericFailure:
            return ("操作失败（错误码 %ld）", "The operation failed (error code %ld)")

        // MARK: 会话事件

        case .eventLinkUp: return ("链路已连接", "Link is up")
        case .eventLinkDown: return ("链路已断开", "Link is down")
        case .eventDeviceResetReplayed:
            return ("设备已软复位，寻址信息已重放",
                    "The device soft-reset; addressing information has been replayed")
        case .eventDeviceReset: return ("设备已软复位", "The device soft-reset")
        case .eventNegotiated:
            return ("RNDIS 协商完成：MTU %1$ld，链路 %2$ld Mbps",
                    "RNDIS negotiation complete: MTU %1$ld, link %2$ld Mbps")

        // MARK: 网络参数校验

        case .invalidIPAddress: return ("IP 地址格式不正确", "The IP address is not valid")
        case .invalidNetmask:
            return ("子网掩码不正确（必须是连续的掩码，如 255.255.255.0）",
                    "The subnet mask is not valid (it must be contiguous, e.g. 255.255.255.0)")
        case .invalidRouter: return ("网关地址格式不正确", "The router address is not valid")
        case .routerRequiredForDefaultRoute:
            return ("要把流量默认走这张网卡，必须填写网关地址",
                    "Routing traffic through this interface by default requires a router address")
        case .invalidDNSServer:
            return ("DNS 服务器 %@ 格式不正确", "DNS server %@ is not a valid address")

        // MARK: helper 服务

        case .helperSessionAlreadyRunning: return ("会话已经在运行了", "A session is already running")
        case .helperSessionStopped: return ("会话已停止", "Session stopped")
        case .helperNetworkApplied:
            return ("已应用网络配置：%@", "Applied the network configuration: %@")
        case .helperRequestDecodeFailed:
            return ("请求参数无法解析：%@", "Cannot decode the request arguments: %@")
        case .helperReplyEncodeFailed: return ("应答编码失败：%@", "Cannot encode the reply: %@")
        case .helperOrphansCleaned:
            return ("已清理 %ld 张上次残留的虚拟网卡",
                    "Cleaned up %ld leftover virtual interface(s) from a previous run")
        case .helperOrphanCleanupFailed:
            return ("清理残留虚拟网卡失败：%@",
                    "Failed to clean up leftover virtual interfaces: %@")
        case .helperSigtermReceived: return ("收到 SIGTERM，正在停机", "Received SIGTERM; shutting down")
        case .helperReady: return ("tetherkit-helper 已就绪：%@", "tetherkit-helper ready: %@")

        // MARK: helper 客户端

        case .helperConnectFailed:
            return ("无法连接到特权组件：%@", "Cannot reach the privileged helper: %@")
        case .helperReplyUnparsable:
            return ("特权组件的应答无法解析，可能是版本不一致",
                    "The helper's reply could not be parsed; the versions may not match")

        // MARK: 检查更新

        case .updateNoReleases: return ("仓库还没有发布任何版本", "The repository has no releases yet")
        case .updateHTTPStatus: return ("GitHub 返回了 %ld", "GitHub returned %ld")
        case .updateBadResponse: return ("响应格式不符合预期", "The response was not in the expected format")


        case .cliLinkNotInBundle:
            return ("命令行工具只能在从 TetherKit.app 注册的后台组件中安装。",
                    "The command-line tool can only be installed by the background component registered from TetherKit.app.")
        case .cliLinkToolMissing:
            return ("App 包内找不到命令行工具：%@",
                    "The command-line tool is missing from the app bundle: %@")
        case .cliLinkDirectoryUnsafe:
            return ("%@ 不是普通目录，出于安全考虑不在其中创建链接。",
                    "%@ is not a regular directory, so no link is created there for safety.")
        case .cliLinkOccupied:
            return ("%@ 已被其他程序占用（例如 Homebrew 安装的 tetherkit-cli），请先移除它。",
                    "%@ is already taken by something else (for example a Homebrew tetherkit-cli). Remove it first.")
        case .cliLinkSystemError:
            return ("%1$@ 失败：%2$@",
                    "%1$@ failed: %2$@")
        case .moveToApplicationsRequired:
            return ("请先把 TetherKit 拖到“应用程序”文件夹再运行。macOS 不允许从磁盘映像或临时位置注册后台组件。",
                    "Move TetherKit to the Applications folder and open it from there first. macOS does not allow background components to be registered from a disk image or a temporary location.")
        case .helperRegisterFailed:
            return ("无法注册后台组件：%@",
                    "Could not register the background component: %@")
        case .authPromptCommandLineTool:
            return ("TetherKit 需要管理员权限来在 /usr/local/bin 中安装或移除 tetherkit-cli 命令。",
                    "TetherKit needs administrator privileges to add or remove the tetherkit-cli command in /usr/local/bin.")
        case .helperApprovalTitle:
            return ("在系统设置中允许 TetherKit",
                    "Allow TetherKit in System Settings")
        case .helperApprovalBody:
            return ("后台组件已注册，但 macOS 需要你确认。请在“系统设置 › 通用 › 登录项与扩展”中打开 TetherKit 的开关，本页会自动继续。",
                    "The background component is registered, but macOS needs your approval. Turn on TetherKit in System Settings › General › Login Items & Extensions; this page continues automatically.")
        case .openLoginItemsSettings:
            return ("打开登录项设置",
                    "Open Login Items Settings")
        case .commandLineToolTitle:
            return ("命令行工具",
                    "Command-line tool")
        case .commandLineToolInstalled:
            return ("已安装：在终端中运行 tetherkit-cli",
                    "Installed: run tetherkit-cli in Terminal")
        case .commandLineToolNotInstalled:
            return ("未安装。安装后可在任何终端中使用 tetherkit-cli。",
                    "Not installed. Install it to use tetherkit-cli from any terminal.")
        case .commandLineToolOccupied:
            return ("%@ 已被其他程序占用",
                    "%@ is used by something else")
        case .installCommandLineTool:
            return ("安装命令",
                    "Install Command")
        case .removeCommandLineTool:
            return ("移除命令",
                    "Remove Command")
        case .needInstallBody:
            return ("创建虚拟网卡和打开数据链路需要管理员权限。TetherKit 把这部分放在一个独立的后台组件里，App 本身以普通用户身份运行。",
                    "Creating the virtual interface and opening the data link require administrator privileges. TetherKit keeps that work in a separate background component so the app itself runs as a normal user.")
        case .installHelperDetail:
            return ("组件随 App 一起签名，直接从 App 内运行，不会复制到系统目录。macOS 会请你在“登录项”中允许它一次。",
                    "The component is signed with the app and runs from inside it; nothing is copied into system folders. macOS asks you to allow it once in Login Items.")
        case .installHelperButton:
            return ("启用后台组件",
                    "Enable Background Component")
        case .installingProgress:
            return ("正在启用……",
                    "Enabling…")
        case .updateHelperDetail:
            return ("将从当前 App 重新启动后台组件，完成后本页自动恢复。",
                    "The background component restarts from this copy of the app. This page recovers on its own afterwards.")
        case .helperVersionMismatchTooltip:
            return ("正在运行的后台组件仍是 %1$@，而 App 已经是 %2$@。点一下即可从当前 App 重新启动它。",
                    "The running background component is still %1$@ while the app is %2$@. One click restarts it from this app.")
        case .helperUpdateWhileRunningWarning:
            return ("重启后台组件会断开当前连接并销毁虚拟网卡，完成后重新连接即可。",
                    "Restarting the background component drops the current connection and destroys the virtual interface. Just reconnect afterwards.")
        case .uninstallExplanation:
            return ("将停用后台组件并移除 tetherkit-cli 命令。之后随时可以重新启用；把 App 移到废纸篓也会一并移除它。",
                    "This disables the background component and removes the tetherkit-cli command. You can enable it again at any time; moving the app to the Trash removes it too.")
        case .updateAvailable:
            return ("发现新版本 v%@。请从发布页下载新的 DMG，并用它替换“应用程序”中的 TetherKit。",
                    "Version v%@ is available. Download the new DMG from the release page and replace TetherKit in your Applications folder.")
        case .uninstallHelperMenuItem:
            return ("停用后台组件…",
                    "Disable Background Component…")
        case .confirmUninstallTitle:
            return ("停用后台组件？",
                    "Disable the background component?")
        case .needInstallTitle:
            return ("启用 TetherKit 后台组件",
                    "Enable the TetherKit background component")
        case .updateHelperButton:
            return ("重启后台组件",
                    "Restart Background Component")
        case .confirmHelperUpdateTitle:
            return ("重启后台组件？",
                    "Restart the background component?")
        case .paneOverview:
            return ("概览",
                    "Overview")
        case .paneDevice:
            return ("设备",
                    "Device")
        case .paneNetwork:
            return ("网络",
                    "Network")
        case .paneActivity:
            return ("日志",
                    "Activity")
        case .paneSettings:
            return ("设置",
                    "Settings")
        case .configureNetwork:
            return ("配置…",
                    "Configure…")
        case .setupTitle:
            return ("欢迎使用 TetherKit",
                    "Welcome to TetherKit")
        case .setupSubtitle:
            return ("只需几步，就能把 Android 手机的 USB 网络共享变成 Mac 上的一张网卡。",
                    "A few steps turn your Android phone's USB tethering into a network interface on your Mac.")
        case .setupStepMoveTitle:
            return ("放入“应用程序”文件夹",
                    "Put TetherKit in Applications")
        case .showInFinder:
            return ("在访达中显示",
                    "Show in Finder")
        case .waitingForApproval:
            return ("正在等待批准……",
                    "Waiting for approval…")
        case .settingsBackgroundSection:
            return ("后台组件",
                    "Background Component")
        case .settingsBackgroundStatus:
            return ("状态",
                    "Status")
        case .backgroundRunning:
            return ("运行中 · %@",
                    "Running · %@")
        case .settingsGeneralSection:
            return ("通用",
                    "General")
        case .menuBarShowSpeed:
            return ("在菜单栏显示实时速率",
                    "Show live speed in the menu bar")
        case .launchAtLogin:
            return ("登录时打开 TetherKit",
                    "Open TetherKit at login")
        case .launchAtLoginFailed:
            return ("无法更改登录项：%@",
                    "Could not change the login item: %@")
        case .settingsUpdatesSection:
            return ("更新",
                    "Updates")
        case .autoCheckUpdates:
            return ("每天自动检查更新",
                    "Check for updates daily")
        case .checkNow:
            return ("立即检查",
                    "Check Now")
        case .currentVersion:
            return ("当前版本 %@",
                    "Current version %@")
        case .settingsAboutSection:
            return ("关于",
                    "About")
        case .projectWebsite:
            return ("项目主页",
                    "Project Website")
        case .showLicenses:
            return ("查看许可证",
                    "Show Licenses")
        case .autoConfigureNetwork:
            return ("连接后自动配置网络",
                    "Configure the network automatically on connect")
        case .autoConfigureNetworkHelp:
            return ("连接成功后立即应用“网络”页选择的方式（默认自动 DHCP），无需再点“应用”。",
                    "Applies the mode chosen on the Network page (DHCP by default) right after connecting, so internet works without another click.")

        // MARK: 连接状态

        case .statusDisconnected: return ("未连接", "Not connected")
        case .statusConnecting: return ("正在连接", "Connecting")
        case .statusConnected: return ("已连接", "Connected")
        case .statusReadyLinkDown: return ("已就绪（链路未连通）", "Ready (link down)")
        case .statusDisconnecting: return ("正在断开", "Disconnecting")
        case .statusStopped: return ("已断开", "Disconnected")
        case .statusFailed: return ("连接失败", "Connection failed")

        // MARK: 应用模型

        case .authPromptSession:
            return ("TetherKit 需要管理员权限来创建虚拟网卡、打开数据链路并配置 IP 地址。",
                    "TetherKit needs administrator privileges to create the virtual interface, "
                    + "open the data link and configure IP addresses.")
        case .interfaceNotReadyYet:
            return ("虚拟网卡还没创建，请先连接设备",
                    "The virtual interface does not exist yet -- connect a device first")

        // MARK: 主界面

        case .menuCheckForUpdates: return ("检查更新…", "Check for Updates…")
        case .alertOperationFailed: return ("操作失败", "Operation failed")
        case .updateCheckTitle: return ("检查更新", "Check for updates")
        case .openReleasePage: return ("前往发布页", "Open the release page")
        case .updateUpToDate:
            return ("当前已是最新版本（v%@）。", "You are on the latest version (v%@).")
        case .updateCheckFailed: return ("无法完成检查：%@", "The check could not be completed: %@")
        case .updateDevBuild:
            return ("这是开发构建（没有版本号），无从比较。",
                    "This is a development build with no version number, so there is nothing to "
                    + "compare against.")
        case .uninstallWhileRunningWarning:
            return ("当前连接会被断开、虚拟网卡销毁。",
                    "The current connection will be dropped and the virtual interface destroyed.")
        case .helperComponentVersion: return ("特权组件 · %@", "Helper · %@")
        case .helperVersionMismatch: return ("特权组件 %1$@ · App %2$@", "Helper %1$@ · app %2$@")
        case .helperUpdateBadge: return ("有新版 %@", "%@ available")
        case .checkingHelper: return ("正在检查特权组件……", "Checking the privileged helper…")
        case .showConnectFailureDetail:
            return ("查看连接失败的详细原因", "Show why the connection failed")
        case .helperNeedsUpdateTitle: return ("特权组件需要更新", "The privileged helper needs updating")
        case .helperNeedsUpdateBody:
            return ("已安装的特权组件是升级前的版本，与当前 App 的通信接口对不上"
                    + "（组件 v%1$@，App 需要 v%2$@）。",
                    "The installed helper predates this app and speaks a different protocol "
                    + "(helper v%1$@, app needs v%2$@).")
        case .copiedToClipboard: return ("已复制到剪贴板", "Copied to the clipboard")
        case .copyCommand: return ("复制命令", "Copy the command")
        case .sysctlNeedsFixTitle: return ("系统参数需要调整", "System parameters need adjusting")
        case .sysctlNeedsFixBody:
            return ("下面这些开关会在虚拟网卡**创建时**被快照进去，创建后再改无效，\n"
                    + "因此必须先修正再连接：",
                    "These switches are snapshotted into the virtual interface **when it is\n"
                    + "created**, so changing them afterwards has no effect. Fix them before\n"
                    + "connecting:")

        // MARK: 设备卡

        case .usbDeviceSectionTitle: return ("USB 设备", "USB device")
        case .cannotRescanWhileRunning:
            return ("运行中无法刷新设备列表", "The device list cannot be refreshed while running")
        case .rescanUSBDevices: return ("重新扫描 USB 设备", "Rescan USB devices")
        case .noDeviceDetected: return ("没有检测到设备", "No device detected")
        case .deviceChecklistCable:
            return ("① 用的是数据线，而不是只能供电的充电线",
                    "1. The cable carries data and is not charge-only")
        case .deviceChecklistTethering:
            return ("② 手机上已打开「USB 网络共享 / USB tethering」",
                    "2. \"USB tethering\" is switched on for the phone")
        case .deviceChecklistUnlocked:
            return ("③ 手机已解锁并信任本机", "3. The phone is unlocked and trusts this Mac")
        case .mtuBytes: return ("%@ 字节", "%@ bytes")
        case .mtuHelp:
            return ("超出设备能力时会在协商阶段自动下调。",
                    "Values beyond the device's capability are lowered during negotiation.")
        case .mtuHelpWithLimit:
            return ("超出设备能力时会在协商阶段自动下调（本机上限 %ld）。",
                    "Values beyond the device's capability are lowered during negotiation "
                    + "(this Mac allows up to %ld).")
        case .mtuTooltip:
            return ("协商阶段以设备汇报的能力为准，填大了会自动下调。"
                    + "上限受系统的 net.link.fake.max_mtu 约束。",
                    "Negotiation honours whatever the device reports, so an oversized value is "
                    + "lowered automatically. The ceiling comes from the system's "
                    + "net.link.fake.max_mtu.")
        case .adoptDeviceMAC: return ("采用设备汇报的 MAC 地址", "Adopt the MAC address the device reports")
        case .adoptDeviceMACHelp:
            return ("只有排查 MAC 冲突时才需要关掉。",
                    "Only turn this off when investigating a MAC address conflict.")
        case .adoptDeviceMACTooltip:
            return ("RNDIS 语义下设备就是这块网卡，对端的 ARP 表与 DHCP 租约都按设备的 MAC 建立。",
                    "Under RNDIS the device *is* the network interface, so the peer's ARP table "
                    + "and DHCP leases are all keyed on the device's MAC.")
        case .compatibilityMode: return ("兼容模式", "Compatibility mode")
        case .compatibilityModeTooltip:
            return ("该设备的 CDC 描述符不规范，已按 Android 的惯例推断接口编号",
                    "This device's CDC descriptors are non-standard, so the interface numbers "
                    + "were inferred using the usual Android layout")

        // MARK: 日志卡

        case .logSectionTitle: return ("运行日志", "Log")
        case .logDroppedNotice:
            return ("有 %ld 条日志因缓冲写满被丢弃", "%ld log lines were dropped because the buffer filled up")
        case .logLevelLabel: return ("级别", "Level")
        case .logLevelAll: return ("全部", "All")
        case .logLevelDebug: return ("调试", "Debug")
        case .logLevelInfo: return ("信息", "Info")
        case .logLevelWarning: return ("警告", "Warning")
        case .logLevelError: return ("错误", "Error")
        case .logAutoScroll: return ("自动滚动", "Auto-scroll")
        case .logCopyAll: return ("复制全部日志", "Copy all log lines")
        case .logClear: return ("清空日志", "Clear the log")
        case .logRepeatSuffix: return ("（×%1$ld，自 %2$@ 起）", " (x%1$ld, since %2$@)")
        case .logRepeatTooltip:
            return ("这句话连续出现了 %1$ld 次，首次在 %2$@",
                    "This line repeated %1$ld times in a row, first at %2$@")

        // MARK: 菜单栏面板

        case .downstreamShort: return ("下行", "Down")
        case .upstreamShort: return ("上行", "Up")
        case .noIPConfigured: return ("未配置 IP", "No IP address")
        case .menuBarNoDevice:
            return ("未检测到设备。用数据线连接手机并开启 USB 网络共享。",
                    "No device detected. Connect a phone with a data cable and turn on USB "
                    + "tethering.")
        case .menuBarReady:
            return ("已就绪：%@。打开主窗口即可连接。",
                    "Ready: %@. Open the main window to connect.")
        case .openMainWindow: return ("打开主窗口", "Open the main window")
        case .quit: return ("退出", "Quit")
        case .quitTooltip:
            return ("退出只是关闭界面，已建立的连接会继续运行。",
                    "Quitting only closes the interface; an established connection keeps running.")

        // MARK: 网络卡

        case .dnsEffectivenessTooltip:
            return ("静态模式下 DNS 是否生效取决于系统的解析器管理，请以「当前生效」里的回读结果为准。",
                    "In manual mode, whether DNS takes effect is up to the system resolver, so "
                    + "trust what \"Currently effective\" reads back.")
        case .ipModeLabel: return ("上网方式", "Configure IPv4")
        case .connectBeforeConfiguring:
            return ("连接设备后才能配置网络", "Connect a device before configuring the network")
        case .dhcpHelp:
            return ("由系统向设备申请地址，DNS 与路由都自动配好",
                    "The system asks the device for an address; DNS and routes are set up "
                    + "automatically")
        case .dhcpTooltip:
            return ("绝大多数手机的 USB 网络共享都自带 DHCP 服务器，这是推荐选项。"
                    + "应用后最多等待 10 秒；超时通常意味着设备侧没有开启网络共享。",
                    "Nearly every phone's USB tethering runs its own DHCP server, so this is the "
                    + "recommended choice. Applying it waits up to 10 seconds; a timeout usually "
                    + "means tethering is not enabled on the device.")
        case .routerOptional: return ("可留空", "Optional")
        case .deleteThisEntry: return ("删除这一条", "Remove this entry")
        case .addDNSServer: return ("添加 DNS 服务器（最多 4 条）", "Add a DNS server (up to 4)")
        case .setDefaultRoute: return ("让所有流量默认走这张网卡", "Route all traffic through this interface")
        case .setDefaultRouteHelp:
            return ("不开启时，只有明确绑定到本网卡的流量走它。",
                    "When off, only traffic explicitly bound to this interface uses it.")
        case .setDefaultRouteTooltip:
            return ("不开启时其余流量仍走当前的主网络。如果本机没有别的可用网络，"
                    + "通常不需要开 —— 系统会自己把它选为主服务。",
                    "When off, the rest of your traffic keeps using the current primary network. "
                    + "If this Mac has no other network available you usually do not need this -- "
                    + "the system picks this interface as the primary service on its own.")
        case .clearConfiguration: return ("撤销配置", "Clear the configuration")
        case .clearConfigurationTooltip:
            return ("等同于 ipconfig set <网卡> NONE，会移除地址与相关路由",
                    "Equivalent to ipconfig set <interface> NONE; removes the address and its "
                    + "routes")
        case .currentlyEffective: return ("当前生效", "Currently effective")
        case .primaryDefaultRoute: return ("主默认路由", "Primary default route")
        case .notEffective: return ("未生效", "Not in effect")
        case .noAddressYet:
            return ("%@ 目前没有 IP 地址。选择上网方式后点「应用」。",
                    "%@ has no IP address yet. Pick a configuration and press Apply.")

        // MARK: 状态卡

        case .linkUp: return ("链路已连通", "Link up")
        case .linkDown: return ("链路未连通", "Link down")
        case .transferPaused: return ("已暂停搬运", "Transfer paused")
        case .pendingDevice: return ("待连接：%@", "Ready to connect: %@")
        case .noRNDISDeviceDetected:
            return ("没有检测到 RNDIS 设备。请用数据线连接手机，并在手机上打开「USB 网络共享」。",
                    "No RNDIS device detected. Connect a phone with a data cable and turn on "
                    + "\"USB tethering\" on the phone.")
        case .interfaceLabel: return ("网卡", "Interface")
        case .interfaceCreating: return ("创建中…", "Creating…")
        case .connectDisabledHint:
            return ("先连接一台开启了 USB 网络共享的设备",
                    "Connect a device with USB tethering switched on first")

        // MARK: 吞吐卡

        case .throughputSectionTitle: return ("吞吐", "Throughput")
        case .throughputLive: return ("实时", "Live")
        case .throughputDownstream: return ("下行（设备 → 本机）", "Down (device -> Mac)")
        case .throughputUpstream: return ("上行（本机 → 设备）", "Up (Mac -> device)")
        case .throughputDownstreamFPS: return ("下行帧率", "Down frame rate")
        case .throughputUpstreamFPS: return ("上行帧率", "Up frame rate")
        case .chartTime: return ("时间", "Time")
        case .chartRate: return ("速率", "Rate")
        case .chartDirection: return ("方向", "Direction")
        case .throughputCollecting: return ("正在采集…", "Collecting…")
        case .throughputPlaceholder:
            return ("连接后显示实时吞吐", "Live throughput appears once you connect")
        case .totalDownstream: return ("累计下行", "Total down")
        case .totalUpstream: return ("累计上行", "Total up")
        case .downstreamFrames: return ("下行帧数", "Down frames")
        case .upstreamFrames: return ("上行帧数", "Up frames")
        case .downstreamDropped: return ("下行丢弃", "Down dropped")
        case .upstreamDropped: return ("上行丢弃", "Up dropped")
        case .kernelDrops: return ("内核丢包", "Kernel drops")
        case .transmitBackpressure: return ("发送背压", "TX backpressure")
        }
    }
}
