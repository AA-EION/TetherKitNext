import AppKit
import SwiftUI
import TetherKitNextIPC

/// TetherKitNext's GUI entry point.
///
/// The app runs as the logged-in user with no privileges; everything that needs
/// root (feth, BPF, IP configuration) is done by the SMAppService daemon over
/// XPC. See docs/GUI-ARCHITECTURE.md.
///
/// ★ Menu-bar-only mode ★
///
///   Closing the main window does not quit: the app drops to the `.accessory`
///   activation policy (no Dock icon, no app menu) and lives on as the menu bar
///   item with live rates. Sessions run in the daemon anyway; the app is only a
///   window onto them. Reopening the window from the menu bar item brings the
///   Dock icon back. Polling keeps going (the menu bar needs it) but slows to
///   2 s while the window is closed and no session is running.
@main
struct TetherKitNextApplication: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var model = AppModel()

    var body: some Scene {
        // Window, not WindowGroup: one connection panel. Several windows would
        // just poll the same daemon several times.
        Window("TetherKitNext", id: WindowID.main) {
            MainWindowRoot(model: model)
        }
        .defaultSize(width: Design.Window.defaultWidth, height: Design.Window.defaultHeight)
        .windowResizability(.contentMinSize)
        .windowToolbarStyle(.unified)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(replacing: .appInfo) { AboutMenuItem(model: model) }
            CommandGroup(after: .appInfo) { AppMenuItems(model: model) }
        }

        MenuBarExtra {
            MenuBarPanel(model: model)
        } label: {
            MenuBarLabel(model: model)
        }
        .menuBarExtraStyle(.window)
    }
}

enum WindowID {
    static let main = "main"
}

/// Keeps the Dock icon in sync with the main window.
///
/// The previous implementation flipped the activation policy from SwiftUI's
/// `onDisappear`, which does not fire reliably when a `Window` scene is closed
/// — the window was gone but the Dock icon stayed. AppKit's window
/// notifications are authoritative, so the policy is derived from them: any
/// visible, non-panel window → `.regular`; none → `.accessory`.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var observers: [NSObjectProtocol] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        let center = NotificationCenter.default
        for name in [NSWindow.willCloseNotification, NSWindow.didBecomeKeyNotification,
                     NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification] {
            observers.append(center.addObserver(forName: name, object: nil,
                                                queue: .main) { notification in
                let window = notification.object as? NSWindow
                let closing = notification.name == NSWindow.willCloseNotification
                MainActor.assumeIsolated {
                    AppDelegate.updateActivationPolicy(excluding: closing ? window : nil)
                }
            })
        }
    }

    /// Menu bar item stays; closing the last window must not quit the app.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    static func updateActivationPolicy(excluding closing: NSWindow? = nil) {
        let hasAppWindow = NSApp.windows.contains { window in
            window !== closing && isAppWindow(window)
                && (window.isVisible || window.isMiniaturized)
        }
        let policy: NSApplication.ActivationPolicy = hasAppWindow ? .regular : .accessory
        guard NSApp.activationPolicy() != policy else { return }
        NSApp.setActivationPolicy(policy)
        if policy == .regular {
            NSApp.activate()
        }
    }

    /// Real document-style windows, as opposed to the menu bar extra's panel,
    /// the status item itself, popovers and alerts' sheets.
    private static func isAppWindow(_ window: NSWindow) -> Bool {
        !(window is NSPanel) && window.canBecomeMain
            && window.styleMask.contains(.titled)
    }
}

/// "About TetherKitNext", replaced so the panel carries the vendor credit and
/// a link to its website.
private struct AboutMenuItem: View {
    var model: AppModel

    var body: some View {
        let _ = model.languageRevision
        Button(L(.aboutApp)) { Vendor.showAboutPanel() }
    }
}

private struct AppMenuItems: View {
    var model: AppModel

    var body: some View {
        // Read the revision so the whole group re-renders on a language switch
        // (see docs/GUI-ARCHITECTURE.md, SwiftUI and global lookup tables).
        let _ = model.languageRevision

        Button(L(.menuCheckForUpdates)) {
            Task { await model.checkForUpdates() }
        }
        LanguageMenu(model: model)
    }
}

struct LanguageMenu: View {
    var model: AppModel

    var body: some View {
        Menu(L(.languageMenuTitle)) {
            LanguagePicker(model: model)
                .pickerStyle(.inline)
                .labelsHidden()
        }
    }
}

struct LanguagePicker: View {
    var model: AppModel

    var body: some View {
        Picker(L(.languageLabel), selection: Bindable(model).languagePreference) {
            Text(L(.languageSystem)).tag(LanguagePreference.system)
            Text(verbatim: "中文").tag(LanguagePreference.chinese)
            Text(verbatim: "English").tag(LanguagePreference.english)
        }
    }
}

private struct MainWindowRoot: View {
    var model: AppModel
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        ContentView(model: model)
            .frame(minWidth: Design.Window.minWidth, minHeight: Design.Window.minHeight)
            .id(model.languageRevision)
            .task {
                model.start()
            }
            .onAppear {
                // SwiftUI resurrects the Window scene when a windowless app is
                // activated (clicking the menu bar item does that). Only keep
                // windows we asked for.
                guard model.isWindowPresentationExpected() else {
                    dismissWindow(id: WindowID.main)
                    return
                }
                model.windowDidAppear()
                AppDelegate.updateActivationPolicy()
            }
            .onDisappear {
                model.windowDidDisappear()
            }
    }
}

/// Opens (or brings forward) the main window from the menu bar.
@MainActor
func presentMainWindow(_ model: AppModel, _ openWindow: OpenWindowAction) {
    model.expectWindowPresentation()
    NSApp.setActivationPolicy(.regular)
    openWindow(id: WindowID.main)
    NSApp.activate()
}
