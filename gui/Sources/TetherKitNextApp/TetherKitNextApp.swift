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

    var body: some Scene {
        // No Window scene: the main window is an AppKit NSWindow owned by
        // MainWindowController (see there for why). The menu bar extra is the
        // only SwiftUI scene; app-wide commands attach to it.
        MenuBarExtra {
            MenuBarPanel(model: appDelegate.model)
        } label: {
            MenuBarLabel(model: appDelegate.model)
        }
        .menuBarExtraStyle(.window)
        .commands {
            CommandGroup(replacing: .newItem) {}
            // Without a Window scene SwiftUI provides no File ▸ Close; ⌘W
            // hides the window like the red button (menu-bar-only mode).
            CommandGroup(after: .newItem) { CloseWindowMenuItem(model: appDelegate.model) }
            CommandGroup(replacing: .appInfo) { AboutMenuItem(model: appDelegate.model) }
            CommandGroup(after: .appInfo) { AppMenuItems(model: appDelegate.model) }
        }
    }
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
    /// The single app model, shared by the main window and the menu bar
    /// extra. Owned here (not by the App struct) because the AppKit-owned
    /// main window is created from `applicationDidFinishLaunching`.
    let model = AppModel()
    private var observers: [NSObjectProtocol] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        model.start()
        MainWindowController.install(model: model).present()

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

    /// Dock icon click or Finder double-click while the app is running.
    /// Reaches us now that SwiftUI owns no window scene; reopen the main
    /// window instead of doing nothing (menu-bar-only mode has no Dock icon,
    /// but a Finder double-click still lands here).
    func applicationShouldHandleReopen(_ sender: NSApplication,
                                       hasVisibleWindows flag: Bool) -> Bool {
        if !(MainWindowController.shared?.isVisible ?? true) {
            MainWindowController.shared?.present()
        }
        return true
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
            Text(L(.languageEnglish)).tag(LanguagePreference.english)
            Text(L(.languageSpanish)).tag(LanguagePreference.spanish)
            Text(L(.languageChinese)).tag(LanguagePreference.chinese)
            Text(L(.languageJapanese)).tag(LanguagePreference.japanese)
            Text(L(.languageGerman)).tag(LanguagePreference.german)
            Text(L(.languageKorean)).tag(LanguagePreference.korean)
        }
    }
}

/// File ▸ Close (⌘W).
private struct CloseWindowMenuItem: View {
    var model: AppModel

    var body: some View {
        let _ = model.languageRevision
        Button(L(.closeWindow)) { MainWindowController.shared?.closeFrontWindow() }
            .keyboardShortcut("w")
    }
}
