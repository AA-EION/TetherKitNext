import SwiftUI
import TetherKitNextIPC

/// The log panel. Resident at the bottom of the right column, consuming all the height left over by the two-column layout -- the larger the window the more
/// can be seen, rather than being folded up waiting for the user to expand it (the old version's folding was a compromise a single-column layout could not fit).
///
/// When something goes wrong it is the only useful thing, so three things were done: filterable by level, automatic scroll to the bottom,
/// and one-click copy of everything (users can paste it directly when reporting an issue).
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
            // Flexible height: consumes the remaining space of the right column. The minimum guarantees that in extreme cases (window pressed to the minimum,
            // the cards above all in their tallest state) a few lines can still be seen, rather than being squeezed into a slit.
            // 90 ~= four lines -- the whole page's budget is "fits even in the minimum window (content height 700)",
            // and after the left column gained the privileged-component management row, this is the only reasonable stretch-absorbing layer.
            //
            // idealHeight must be pinned: outside the whole page there is a backstop ScrollView, which measures content
            // using ideal sizes, and the ideal height of a nested ScrollView = the height of all logs expanded
            // -- if not pinned, once there are many logs the whole page gets stretched open, and scrolling comes back.
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
                // A fixed width keeps the level column aligned, so the eye need not search horizontally when skimming.
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
