import AppKit
import SwiftUI
import TetherKitIPC

/// Small views shared by several pages.

struct EnvironmentWarningCard: View {
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
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: Design.Radius.control, style: .continuous))
    }
}
