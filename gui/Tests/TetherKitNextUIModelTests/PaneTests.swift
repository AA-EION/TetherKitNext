import XCTest

@testable import TetherKitNextUIModel

/// Sidebar composition and persisted-selection rules.
///
/// Failure modes guarded here are silent in the UI: an advanced page left
/// selected after the user turns advanced tools off would open the window on
/// a page the sidebar no longer lists, with no way to see which row is active.
final class PaneTests: XCTestCase {

    func testAdvancedPagesHiddenByDefault() {
        let panes = Pane.sidebarSections(advancedToolsEnabled: false).flatMap { $0 }
        XCTAssertFalse(panes.contains(where: \.isAdvanced))
        XCTAssertEqual(Pane.sidebarSections(advancedToolsEnabled: false).count, 2)
    }

    func testAdvancedSectionSitsBetweenEverydayPagesAndSettings() {
        let sections = Pane.sidebarSections(advancedToolsEnabled: true)
        XCTAssertEqual(sections.count, 3)
        XCTAssertEqual(sections.first?.first, .overview)
        XCTAssertTrue(sections[1].allSatisfy(\.isAdvanced))
        XCTAssertEqual(sections.last, [.settings])
    }

    func testEveryPaneIsReachableWhenAdvancedToolsAreOn() {
        let listed = Set(Pane.sidebarSections(advancedToolsEnabled: true).flatMap { $0 })
        XCTAssertEqual(listed, Set(Pane.allCases))
    }

    func testResolveStoredFallsBackToOverview() {
        XCTAssertEqual(Pane.resolveStored(nil, advancedToolsEnabled: true), .overview)
        XCTAssertEqual(Pane.resolveStored("", advancedToolsEnabled: true), .overview)
        XCTAssertEqual(Pane.resolveStored("renamedPage", advancedToolsEnabled: true), .overview)
    }

    func testResolveStoredKeepsVisiblePages() {
        XCTAssertEqual(Pane.resolveStored("network", advancedToolsEnabled: false), .network)
        XCTAssertEqual(Pane.resolveStored("interfaces", advancedToolsEnabled: true), .interfaces)
    }

    func testResolveStoredDropsHiddenAdvancedPage() {
        XCTAssertEqual(Pane.resolveStored("interfaces", advancedToolsEnabled: false), .overview)
    }

    /// The persisted raw values are a storage format: renaming one silently
    /// resets everyone's selection once.
    func testRawValuesAreStable() {
        XCTAssertEqual(Pane.allCases.map(\.rawValue),
                       ["overview", "device", "network", "activity", "interfaces", "settings"])
    }
}
