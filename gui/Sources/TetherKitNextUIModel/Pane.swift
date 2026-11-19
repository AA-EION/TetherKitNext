import TetherKitNextIPC

/// Sidebar destinations of the main window.
///
/// A library target of its own (not part of `TetherKitNextApp`) because a
/// SwiftPM executable target cannot be `@testable`-imported: the selection
/// rules below are exactly the kind of thing that breaks silently (a page
/// that cannot be reached, a stale selection pointing at a hidden page), so
/// they are pinned by `TetherKitNextUIModelTests`.
///
/// The selection is persisted by `rawValue` (`@AppStorage`). Renaming a case
/// drops a stored selection once (it falls back to `.overview`); adding or
/// reordering cases is harmless.
public enum Pane: String, CaseIterable, Identifiable, Hashable, Sendable {
    case overview, device, network, activity
    /// Advanced tools: only listed when the user opts in (Settings → General).
    case interfaces
    case settings

    public var id: Self { self }

    public var titleKey: L10nKey {
        switch self {
        case .overview: return .paneOverview
        case .device: return .paneDevice
        case .network: return .paneNetwork
        case .activity: return .paneActivity
        case .interfaces: return .paneInterfaces
        case .settings: return .paneSettings
        }
    }

    public var title: String { L(titleKey) }

    public var symbol: String {
        switch self {
        case .overview: return "gauge.with.dots.needle.67percent"
        case .device: return "cable.connector"
        case .network: return "network"
        case .activity: return "list.bullet.rectangle"
        case .interfaces: return "point.3.connected.trianglepath.dotted"
        case .settings: return "gearshape"
        }
    }

    /// Pages that belong to the opt-in "Advanced" sidebar section.
    ///
    /// Everyday users of the app never see these unless they turn them on, so
    /// diagnostic tooling can grow here without cluttering the tethering flow.
    public var isAdvanced: Bool {
        switch self {
        case .interfaces: return true
        case .overview, .device, .network, .activity, .settings: return false
        }
    }

    /// The sidebar, top to bottom: everyday pages, the optional advanced
    /// section, then Settings on its own. Empty sections are omitted.
    public static func sidebarSections(advancedToolsEnabled: Bool) -> [[Pane]] {
        let everyday: [Pane] = [.overview, .device, .network, .activity]
        let advanced = advancedToolsEnabled ? allCases.filter(\.isAdvanced) : []
        return [everyday, advanced, [.settings]].filter { !$0.isEmpty }
    }

    /// Resolves a persisted selection.
    ///
    /// Unknown or empty values (a renamed case, a fresh install) and advanced
    /// pages while advanced tools are turned off all land on `.overview`: the
    /// window must never open on a page the sidebar does not list.
    public static func resolveStored(_ rawValue: String?, advancedToolsEnabled: Bool) -> Pane {
        guard let rawValue, let pane = Pane(rawValue: rawValue) else { return .overview }
        if pane.isAdvanced && !advancedToolsEnabled { return .overview }
        return pane
    }
}
