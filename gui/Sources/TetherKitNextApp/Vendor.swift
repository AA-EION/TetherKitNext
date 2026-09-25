import AppKit
import TetherKitNextIPC

/// Who makes TetherKitNext, and where to find them.
enum Vendor {
    static let name = "Issen Software Group"
    static let website = URL(string: "https://issen.kurokamicorp.com/")!

    /// Shows the standard About panel with the vendor credit and a link to its
    /// website underneath the version (the panel reads name, version and
    /// copyright from Info.plist by itself).
    @MainActor
    static func showAboutPanel() {
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = .center
        let base: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize),
            .foregroundColor: NSColor.secondaryLabelColor,
            .paragraphStyle: paragraph,
        ]
        let credits = NSMutableAttributedString(string: L(.aboutCredits) + "\n", attributes: base)
        var link = base
        link[.link] = website
        credits.append(NSAttributedString(string: website.host() ?? website.absoluteString,
                                          attributes: link))

        NSApp.activate()
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
    }
}
