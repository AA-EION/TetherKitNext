import SwiftUI
import TetherKitIPC

/// 主界面。
///
/// 布局目标是**默认窗口尺寸下一屏放完，不滚动**：常驻仪表盘要靠滚动才能看全，
/// 等于把「瞟一眼」变成了「翻一遍」。为此分成两栏 ——
///
///   顶部横幅：现在什么状态（全宽，唯一的操作重心）
///   左栏「控制」：用哪台设备 → 怎么上网（连接前用，连上后基本不碰）
///   右栏「观测」：跑得怎么样 → 出了什么事（连上后 90% 的时间只看这边）
///
/// 日志不再折叠：它常驻右栏底部，把剩余高度全部吃掉 —— 窗口越大看到的越多，
/// 而不是留一片空白还要用户手动展开。
///
/// 外层仍留一个 ScrollView 兜底：极端组合（比如把窗口压到最小 + 静态表单 +
/// 系统参数警告同时出现）超出可视区时宁可滚动也不能截断。`basedOnSize` 保证
/// 内容放得下时它完全不参与 —— 不出滚动条也不回弹。
///
/// 刻意不用 NavigationSplitView 的侧边栏：这个 App 只有一件事可做，
/// 侧边栏只会制造一个空荡荡的导航层级。
struct ContentView: View {
    @Bindable var model: AppModel

    /// 卸载是破坏性动作（断开连接、删系统文件），必须先确认。
    @State private var confirmingHelperUninstall = false

    /// 更新组件时**正跑着会话**才需要确认 —— 安装脚本会先 bootout 旧组件，
    /// 那一下会把连接掐掉。空闲时更新什么都不会毁，再拦一道只会给本来就该
    /// 尽快做的事平添阻力。
    @State private var confirmingHelperUpdate = false

    var body: some View {
        GeometryReader { proxy in
            ScrollView {
                Group {
                    switch model.helperAvailability {
                    case .unknown:
                        ProbingCard()
                    case .missing(let reason):
                        HelperMissingCard(model: model, reason: reason)
                    case .outdated(let installed, let expected):
                        HelperOutdatedCard(model: model, installed: installed,
                                           expected: expected)
                    case .available:
                        dashboard
                    }
                }
                .padding(Design.Spacing.medium)
                // 撑满可视区：右栏的日志靠这个「多余高度」长到窗口底部。
                .frame(minHeight: proxy.size.height, alignment: .top)
            }
            .scrollBounceBehavior(.basedOnSize)
        }
        .background(backgroundGradient)
        .animation(.smooth(duration: 0.25), value: model.status.runState)
        .animation(.smooth(duration: 0.25), value: model.helperAvailability)
        .alert(L(.alertOperationFailed),
               isPresented: Binding(get: { model.alertMessage != nil },
                                    set: { if !$0 { model.alertMessage = nil } })) {
            Button(L(.ok)) { model.alertMessage = nil }
        } message: {
            Text(model.alertMessage ?? "")
        }
        .confirmationDialog(L(.confirmUninstallTitle), isPresented: $confirmingHelperUninstall) {
            Button(L(.uninstall), role: .destructive) {
                Task { await model.uninstallHelper() }
            }
            Button(L(.cancel), role: .cancel) {}
        } message: {
            Text(uninstallWarning)
        }
        .confirmationDialog(L(.confirmHelperUpdateTitle), isPresented: $confirmingHelperUpdate) {
            Button(L(.updateHelperButton)) {
                Task { await model.installHelper() }
            }
            Button(L(.cancel), role: .cancel) {}
        } message: {
            Text(L(.helperUpdateWhileRunningWarning))
        }
        .alert(L(.updateCheckTitle),
               isPresented: Binding(get: { model.updateCheckResult != nil },
                                    set: { if !$0 { model.updateCheckResult = nil } })) {
            if case .updateAvailable(let release) = model.updateCheckResult {
                Button(L(.openReleasePage)) { NSWorkspace.shared.open(release.pageURL) }
                Button(L(.ok), role: .cancel) {}
            } else {
                Button(L(.ok), role: .cancel) {}
            }
        } message: {
            Text(updateCheckDescription)
        }
    }

    /// 「检查更新」弹窗的正文。
    ///
    /// brew 命令名与 tap 里实际的 GUI formula（tetherkit）一致 —— CLI 更名
    /// tetherkit-cli 时这个名字让给了 GUI；手动构建的用户按第二句走。
    private var updateCheckDescription: String {
        switch model.updateCheckResult {
        case .upToDate(let current):
            return L(.updateUpToDate, current)
        case .updateAvailable(let release):
            return L(.updateAvailable, release.version)
        case .failed(let reason):
            return L(.updateCheckFailed, reason)
        case .unavailable:
            return L(.updateDevBuild)
        case nil:
            return ""
        }
    }

    private var uninstallWarning: String {
        let base = L(.uninstallExplanation)
        return model.status.runState == .running
            ? L(.uninstallWhileRunningWarning) + base
            : base
    }

    private var dashboard: some View {
        VStack(spacing: Design.Spacing.gutter) {
            StatusHeroCard(model: model)
            EnvironmentWarningCard(environment: model.environment)

            HStack(alignment: .top, spacing: Design.Spacing.gutter) {
                // 管理行放在卡片组之外、间距归零：它跟着 gutter 排的话要
                // 白付两个 12pt 的间距 —— 整页的高度预算是「最小窗口
                // （内容高 700）也放得下」，为一行 caption 花 41pt 曾把
                // 预算顶破 14pt，主页面因此能滚一点（实测踩过）。
                // Spacer 把它压到左栏底部的既有空隙里，不新增整页高度。
                VStack(spacing: 0) {
                    VStack(spacing: Design.Spacing.gutter) {
                        DeviceCard(model: model)
                        NetworkCard(model: model)
                    }
                    Spacer(minLength: 0)
                    helperManagementFooter
                        .padding(.top, Design.Spacing.tight)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)

                VStack(spacing: Design.Spacing.gutter) {
                    ThroughputCard(model: model)
                    LogCard(model: model)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            }
        }
    }

    /// 左栏底部的特权组件管理行。
    ///
    /// 刻意做得低调（caption + borderless）：卸载是极低频的管理动作，不该
    /// 从正常使用里抢走任何注意力 —— 但它必须存在于主界面，否则组件
    /// 「装得上、删不掉」，只能回终端翻脚本。
    @ViewBuilder
    private var helperManagementFooter: some View {
        if case .available(let version) = model.helperAvailability {
            HStack(spacing: Design.Spacing.small) {
                if let mismatch = model.helperVersionMismatch {
                    // 版本对不上时这一行从「参考信息」变成「有件事等着你做」：
                    // 两个版本号一起摆出来，颜色也从 tertiary 提到 orange。
                    Text(L(.helperVersionMismatch, mismatch.installed, mismatch.expected))
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .textSelection(.enabled)
                        .lineLimit(1)
                    // 忙的时候换成「正在安装……」而不是只把按钮置灰：授权框关掉
                    // 之后要等好几秒（bootout → 拷文件 → bootstrap → 探活），
                    // 一个灰掉的按钮看起来像是点了没反应。
                    Button(model.isBusy ? L(.installingProgress) : L(.updateHelperButton)) {
                        requestHelperUpdate()
                    }
                    .buttonStyle(.borderless)
                    .font(.caption)
                    .foregroundStyle(.orange)
                    .disabled(model.isBusy)
                    .help(L(.helperVersionMismatchTooltip, mismatch.installed, mismatch.expected))
                } else {
                    Text(L(.helperComponentVersion, version))
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .textSelection(.enabled)
                        .lineLimit(1)
                    // 塞在既有行里而不是另起一行：整页预算 700pt 已经没有余粮
                    // 给新行了（管理行自己就险些顶破过一次）。
                    //
                    // 与版本不一致互斥，也是同一个理由 —— 一行放不下两组提示。
                    // 让位的是这条：它只是「知道一下」（升级要去终端敲 brew），
                    // 而版本不一致是当场点一下就能解决的。
                    if let update = model.availableUpdate {
                        Button(L(.helperUpdateBadge, update.version)) {
                            model.updateCheckResult = .updateAvailable(update)
                        }
                        .buttonStyle(.borderless)
                        .font(.caption)
                        .foregroundStyle(Color.accentColor)
                    }
                }
                Spacer()
                Button(L(.uninstallHelperMenuItem)) { confirmingHelperUninstall = true }
                    .buttonStyle(.borderless)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .disabled(model.isBusy)
            }
            .padding(.horizontal, Design.Spacing.tight)
        }
    }

    /// 「更新特权组件」被点了。跑着会话才先确认 —— 见 `confirmingHelperUpdate`。
    private func requestHelperUpdate() {
        if model.status.runState == .running {
            confirmingHelperUpdate = true
        } else {
            Task { await model.installHelper() }
        }
    }

    /// 背景用一层随状态变化的极淡渐变。
    ///
    /// 强度刻意压得很低（0.10 / 0.04）：它的作用是让「连上了」这件事在余光里
    /// 也能被感知，而不是抢内容的注意力。
    private var backgroundGradient: some View {
        let accent = Design.accent(for: model.status.runState)
        return LinearGradient(
            colors: [accent.opacity(0.10), accent.opacity(0.04), .clear],
            startPoint: .top,
            endPoint: .bottom)
        .ignoresSafeArea()
    }
}

/// 正在探测 helper 时的占位。
private struct ProbingCard: View {
    var body: some View {
        Card {
            HStack(spacing: Design.Spacing.small) {
                ProgressView()
                    .controlSize(.small)
                Text(L(.checkingHelper))
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .center)
            .padding(.vertical, Design.Spacing.large)
        }
    }
}

/// helper 没装时的引导。
///
/// 这是新用户最可能撞上的一屏，所以把「为什么需要」说清楚之后直接给一个
/// 「安装」按钮 —— 弹一次系统授权框就装好。终端方案降级成折叠里的备选。
private struct HelperMissingCard: View {
    @Bindable var model: AppModel
    let reason: String

    var body: some View {
        Card(title: L(.needInstallTitle), systemImage: "lock.shield") {
            VStack(alignment: .leading, spacing: Design.Spacing.medium) {
                Text(L(.needInstallBody))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HelperInstallSection(
                    model: model,
                    buttonTitle: L(.installHelperButton),
                    detail: L(.installHelperDetail))

                DisclosureGroup(L(.showConnectFailureDetail)) {
                    Text(reason)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.top, Design.Spacing.tight)
                }
                .font(.caption)
            }
        }
    }
}

/// 装着的 helper 与当前 App 的 XPC 接口对不上。
///
/// 单独一屏而不是复用「没安装」：这两种情况下用户要做的事不一样，而且如果不
/// 明说，症状会表现成调用卡住或直接崩溃 —— 那是最难自查的一类问题。
private struct HelperOutdatedCard: View {
    @Bindable var model: AppModel
    let installed: Int
    let expected: Int

    var body: some View {
        Card(title: L(.helperNeedsUpdateTitle), systemImage: "arrow.triangle.2.circlepath") {
            VStack(alignment: .leading, spacing: Design.Spacing.medium) {
                Text(L(.helperNeedsUpdateBody, String(installed), String(expected)))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                HelperInstallSection(
                    model: model,
                    buttonTitle: L(.updateHelperButton),
                    detail: L(.updateHelperDetail))
            }
        }
    }
}

/// 「一键安装」按钮 + 终端备选方案。missing / outdated 两张卡共用。
///
/// 终端方案必须保留而不是彻底删掉：`swift run` 跑的裸可执行文件没有内嵌载荷，
/// 按钮会明确报错引导到脚本；也总有人就是不愿意往 App 弹的框里输密码。
private struct HelperInstallSection: View {
    @Bindable var model: AppModel
    let buttonTitle: String
    let detail: String

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.small) {
            HStack(spacing: Design.Spacing.small) {
                Button {
                    Task { await model.installHelper() }
                } label: {
                    HStack(spacing: Design.Spacing.tight) {
                        if model.isBusy {
                            ProgressView().controlSize(.small)
                        } else {
                            Image(systemName: "arrow.down.circle.fill")
                        }
                        Text(model.isBusy ? L(.installingProgress) : buttonTitle)
                    }
                    .frame(minWidth: 132)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(model.isBusy)
            }

            Text(detail)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

        }
    }
}

/// 一条可复制的命令。
struct CopyableCommand: View {
    let command: String
    @State private var copied = false

    var body: some View {
        HStack(spacing: Design.Spacing.small) {
            Text(command)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)

            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(command, forType: .string)
                copied = true
                // 2 秒后复原。给的是「已复制」这个确认，不是一个需要用户操作的状态。
                Task {
                    try? await Task.sleep(for: .seconds(2))
                    copied = false
                }
            } label: {
                Label(L(copied ? .copied : .copy), systemImage: copied ? "checkmark" : "doc.on.doc")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(L(copied ? .copiedToClipboard : .copyCommand))
        }
        .padding(Design.Spacing.small)
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: Design.Radius.control))
    }
}

/// 环境预检不合格时的警告。合格时不显示 —— 一切正常时不该占用户的注意力。
private struct EnvironmentWarningCard: View {
    let environment: EnvironmentReport?

    var body: some View {
        if let environment, !environment.sysctlsOK {
            Card(title: L(.sysctlNeedsFixTitle), systemImage: "exclamationmark.triangle.fill") {
                VStack(alignment: .leading, spacing: Design.Spacing.small) {
                    Text(L(.sysctlNeedsFixBody))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Text(environment.sysctlDetail)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(Design.Spacing.small)
                        .background(.quaternary.opacity(0.5),
                                    in: RoundedRectangle(cornerRadius: Design.Radius.control))
                }
            }
        }
    }
}
