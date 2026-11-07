import AppKit
import SwiftUI
import TetherKitNextIPC

/// Owns the main window as a plain AppKit `NSWindow`.
///
/// ★ Why not a SwiftUI `Window` scene ★
///
///   * With a `MenuBarExtra` alongside, SwiftUI recreates a closed `Window`
///     scene whenever the windowless app is activated, which clicking the menu
///     bar item does. Dismissing the resurrected window after the fact still let
///     it flash. With no window scene, SwiftUI has nothing to resurrect.
///   * Presentation becomes deterministic: the window exists from
///     `applicationDidFinishLaunching`, also for bare `swift run` binaries, and
///     `applicationShouldHandleReopen` (Dock / Finder double-click while in
///     menu-bar-only mode) reaches us, so reopening brings it back.
///
///   SwiftUI still renders everything inside it: an `NSHostingController`
///   with toolbar and title bridging, so `.toolbar` / `.navigationTitle` in
///   ContentView keep working.
///
/// ★ Closing ★
///
///   `isReleasedWhenClosed = false`: the red button, File ▸ Close (⌘W) and
///   `close()` only order the window out. The SwiftUI view tree stays alive
///   (cheap, and reopening is instant); AppDelegate's window notifications
///   drop the app to the `.accessory` policy (menu-bar-only mode).
///
/// ★ Size stability ★
///
///   `sizingOptions = [.minSize]`: the hosting controller pushes only the
///   content's minimum size into the window. Its default also publishes the
///   preferred (intrinsic) size, and pages have different intrinsic heights,
///   so switching pages would resize the window, which macOS 26 animates in
///   step with the content change. Set before the controller becomes the
///   window's content, so the first layout already uses it.
@MainActor
final class MainWindowController: NSObject, NSWindowDelegate {
    /// Set once at launch by AppDelegate; nil only before that.
    private(set) static var shared: MainWindowController?

    private let model: AppModel
    private let window: NSWindow

    /// Frame autosave name: the window reopens where the user left it.
    private static let autosaveName = "TetherKitNextMainWindow"

    static func install(model: AppModel) -> MainWindowController {
        if let shared { return shared }
        let controller = MainWindowController(model: model)
        shared = controller
        return controller
    }

    private init(model: AppModel) {
        self.model = model

        let hosting = NSHostingController(rootView: MainWindowRoot(model: model))
        hosting.sizingOptions = [.minSize]
        hosting.sceneBridgingOptions = [.toolbars, .title]

        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0,
                                width: Design.Window.defaultWidth,
                                height: Design.Window.defaultHeight),
            styleMask: [.titled, .closable, .miniaturizable, .resizable,
                        .fullSizeContentView],
            backing: .buffered,
            defer: false)
        window.title = "TetherKitNext"
        window.toolbarStyle = .unified
        window.isReleasedWhenClosed = false
        window.contentMinSize = NSSize(width: Design.Window.minWidth,
                                       height: Design.Window.minHeight)
        window.contentViewController = hosting
        // contentViewController adopts the controller's view size; put the
        // design size back before restoring any saved frame.
        window.setContentSize(NSSize(width: Design.Window.defaultWidth,
                                     height: Design.Window.defaultHeight))
        window.center()
        window.setFrameAutosaveName(Self.autosaveName)

        super.init()
        window.delegate = self
    }

    var isVisible: Bool { window.isVisible || window.isMiniaturized }

    /// Shows the window and brings the app forward. Idempotent: safe from
    /// launch, the menu bar panel, and Dock / Finder reopen alike.
    func present() {
        if NSApp.activationPolicy() != .regular {
            NSApp.setActivationPolicy(.regular)
        }
        if window.isMiniaturized {
            window.deminiaturize(nil)
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate()
    }

    /// File ▸ Close (⌘W). A minimized main window is never key, so
    /// `NSApp.keyWindow?.performClose` alone would do nothing for it; fall
    /// back to closing the main window when nothing else is key.
    func closeFrontWindow() {
        if let key = NSApp.keyWindow {
            key.performClose(nil)
        } else if window.isMiniaturized {
            window.close()
        }
    }

    // MARK: NSWindowDelegate

    func windowDidBecomeKey(_ notification: Notification) {
        model.windowDidAppear()
    }

    func windowWillClose(_ notification: Notification) {
        model.windowDidDisappear()
    }

    func windowDidMiniaturize(_ notification: Notification) {
        model.windowDidDisappear()
    }

    func windowDidDeminiaturize(_ notification: Notification) {
        model.windowDidAppear()
    }
}

/// The SwiftUI content of the main window.
private struct MainWindowRoot: View {
    var model: AppModel

    var body: some View {
        ContentView(model: model)
            .frame(minWidth: Design.Window.minWidth, minHeight: Design.Window.minHeight)
            // Rebuild on a language switch (SwiftUI cannot see the global
            // string table change; docs/GUI-ARCHITECTURE.md).
            .id(model.languageRevision)
    }
}
