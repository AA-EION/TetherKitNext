import SwiftUI
import TetherKitNextIPC

/// 日志面板。常驻右栏底部，把两栏布局剩下的高度全部吃掉 —— 窗口越大看到的
/// 越多，而不是折叠起来等用户展开（旧版折叠是单列布局塞不下的妥协）。
///
/// 出问题时它是唯一有用的东西，所以做了三件事：可按级别过滤、自动滚到底、
/// 一键复制全部（用户报 issue 时能直接贴过来）。
struct LogCard: View {
    @Bindable var model: AppModel
    @State private var autoScroll = true

    var body: some View {
        Card(title: L(.logSectionTitle), systemImage: "text.alignleft") {
            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                toolbar
                logList
                if model.droppedLogCount > 0 {
                    Label(L(.logDroppedNotice, Int(model.droppedLogCount)),
                          systemImage: "exclamationmark.triangle")
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }
        }
    }

    private var toolbar: some View {
        HStack(spacing: Design.Spacing.small) {
            Picker(L(.logLevelLabel), selection: $model.logLevelFilter) {
                Text(L(.logLevelAll)).tag(LogLevel.trace)
                Text(L(.logLevelDebug)).tag(LogLevel.debug)
                Text(L(.logLevelInfo)).tag(LogLevel.info)
                Text(L(.logLevelWarning)).tag(LogLevel.warning)
                Text(L(.logLevelError)).tag(LogLevel.error)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(maxWidth: 280)

            Spacer()

            Toggle(L(.logAutoScroll), isOn: $autoScroll)
                .toggleStyle(.checkbox)
                .font(.callout)

            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(plainText, forType: .string)
            } label: {
                Image(systemName: "doc.on.doc")
            }
            .buttonStyle(.borderless)
            .help(L(.logCopyAll))

            Button {
                model.clearLogs()
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .help(L(.logClear))
        }
    }

    private var logList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    ForEach(model.filteredLogs) { entry in
                        LogRow(entry: entry)
                            .id(entry.id)
                    }
                }
                .padding(Design.Spacing.small)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            // 高度弹性：吃掉右栏剩余空间。最小值保证极端情况下（窗口压到最小、
            // 上面的卡都在最高状态）仍能看到几行，而不是被挤成一条缝。
            // 90 ≈ 四行 —— 整页的预算是「最小窗口（内容高 700）也放得下」，
            // 左栏加了特权组件管理行之后，这里是唯一合理的伸缩吸收层。
            //
            // idealHeight 必须钉死：整页外面有一层兜底 ScrollView，它测量内容
            // 用的是理想尺寸，而内嵌 ScrollView 的理想高度 = 全部日志展开的高度
            // —— 不钉住的话日志一多整页就被撑开，滚动又回来了。
            .frame(minHeight: 90, idealHeight: 90, maxHeight: .infinity)
            .background(.quaternary.opacity(0.35),
                        in: RoundedRectangle(cornerRadius: Design.Radius.control))
            .onChange(of: model.filteredLogs.last?.id) { _, newValue in
                guard autoScroll, let newValue else { return }
                withAnimation(.easeOut(duration: 0.15)) {
                    proxy.scrollTo(newValue, anchor: .bottom)
                }
            }
        }
    }

    private var plainText: String {
        model.filteredLogs
            .map { item in
                var line = "\(Format.time(item.latest.timestamp)) [\(item.latest.level.label)] "
                    + "[\(item.latest.thread)] \(item.latest.message)"
                if item.count > 1 {
                    line += L(.logRepeatSuffix, item.count, Format.time(item.first.timestamp))
                }
                return line
            }
            .joined(separator: "\n")
    }
}

private struct LogRow: View {
    let entry: CollapsedLogEntry

    var body: some View {
        HStack(alignment: .top, spacing: Design.Spacing.small) {
            Text(Format.time(entry.latest.timestamp))
                .foregroundStyle(.tertiary)
            Text(entry.latest.level.label)
                .foregroundStyle(Design.logColor(for: entry.latest.level))
                // 固定宽度让级别列对齐，扫读时眼睛不用横向找。
                .frame(width: 46, alignment: .leading)
            Text(entry.latest.message)
                .foregroundStyle(Design.logColor(for: entry.latest.level))
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)

            if entry.count > 1 {
                Text("×\(entry.count)")
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 1)
                    .background(.quaternary, in: Capsule())
                    .help(L(.logRepeatTooltip, entry.count,
                            Format.time(entry.first.timestamp)))
            }
        }
        .font(.system(size: 11, design: .monospaced))
    }
}
