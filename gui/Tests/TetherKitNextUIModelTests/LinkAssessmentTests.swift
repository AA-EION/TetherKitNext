import XCTest

import TetherKitNextIPC
@testable import TetherKitNextUIModel

/// The link assessment decides whether the app talks about "internet" or a
/// "local device". These cases pin the rules that keep it from misjudging a
/// slow phone as a local device, and a local device as a broken phone.
final class LinkAssessmentTests: XCTestCase {

    private func network(address: String = "", router: String = "") -> NetworkState {
        NetworkState(hasAddress: !address.isEmpty, address: address, netmask: "255.255.255.0",
                     router: router, dnsServers: [], method: "DHCP", serviceState: "",
                     hasDefaultRoute: !router.isEmpty, isPrimaryDefaultRoute: false)
    }

    private func assess(_ seconds: TimeInterval, mode: IPMode = .dhcp,
                        _ state: NetworkState, peer: String? = nil) -> LinkAssessment {
        LinkAssessment.assess(isRunning: true, connectedFor: seconds, mode: mode,
                              network: state, peerAddress: peer)
    }

    func testNotRunning() {
        XCTAssertEqual(LinkAssessment.assess(isRunning: false, connectedFor: 100, mode: .dhcp,
                                             network: network(address: "10.0.0.2"),
                                             peerAddress: nil),
                       .notConnected)
    }

    func testLeaseWithRouterIsInternet() {
        let result = assess(3, network(address: "172.19.0.5", router: "172.19.0.1"))
        XCTAssertEqual(result, .internet(address: "172.19.0.5", router: "172.19.0.1"))
        XCTAssertTrue(result.canCarryInternet)
        XCTAssertFalse(result.isLocalOnly)
    }

    /// A slow phone: nothing yet within the grace period must read "waiting",
    /// never "local device".
    func testSlowDhcpIsWaitingDuringGrace() {
        XCTAssertEqual(assess(5, network()), .waitingForAddress)
        XCTAssertEqual(assess(LinkAssessment.gracePeriod - 0.1, network()), .waitingForAddress)
    }

    /// macOS self-assigns 169.254.x.x while DHCP retries; early on that is
    /// still "waiting".
    func testEarlySelfAssignedAddressIsStillWaiting() {
        XCTAssertEqual(assess(8, network(address: "169.254.3.4")), .waitingForAddress)
    }

    func testSelfAssignedAfterGraceIsLocalLink() {
        XCTAssertEqual(assess(40, network(address: "169.254.3.4"), peer: "169.254.9.9"),
                       .localLink(address: "169.254.3.4", peer: "169.254.9.9", selfAssigned: true))
    }

    /// A device whose DHCP server hands out an address but no gateway.
    func testLeaseWithoutRouterIsLocalLinkImmediately() {
        let result = assess(2, network(address: "192.168.42.10"), peer: "192.168.42.1")
        XCTAssertEqual(result,
                       .localLink(address: "192.168.42.10", peer: "192.168.42.1", selfAssigned: false))
        XCTAssertFalse(result.canCarryInternet)
        XCTAssertTrue(result.isLocalOnly)
    }

    func testNoAddressAfterGraceSuggestsNeighbourOfPeer() {
        XCTAssertEqual(assess(30, network(), peer: "192.168.7.2"),
                       .noAddress(peer: "192.168.7.2", suggestion: "192.168.7.10"))
        XCTAssertEqual(assess(30, network(), peer: nil), .noAddress(peer: nil, suggestion: nil))
        XCTAssertEqual(assess(30, network(), peer: ""), .noAddress(peer: nil, suggestion: nil))
    }

    /// Live re-evaluation: a late lease flips the result without any state.
    func testLateLeaseFlipsToInternet() {
        XCTAssertEqual(assess(45, network(), peer: "172.19.0.1").isLocalOnly, true)
        XCTAssertEqual(assess(46, network(address: "172.19.0.5", router: "172.19.0.1")),
                       .internet(address: "172.19.0.5", router: "172.19.0.1"))
    }

    /// A manual address chosen by the user: no waiting, judged by the router.
    func testManualWithoutRouterIsLocalLink() {
        XCTAssertEqual(assess(1, mode: .manual, network(address: "192.168.1.50")),
                       .localLink(address: "192.168.1.50", peer: nil, selfAssigned: false))
        XCTAssertEqual(assess(1, mode: .manual, network()), .noAddress(peer: nil, suggestion: nil))
    }

    func testDoNotConfigure() {
        XCTAssertEqual(assess(60, mode: .none, network()), .notConfigured)
    }

    func testSuggestedHostAddress() {
        XCTAssertEqual(LinkAssessment.suggestedHostAddress(forPeer: "192.168.42.10"), "192.168.42.20")
        XCTAssertEqual(LinkAssessment.suggestedHostAddress(forPeer: "10.1.2.3"), "10.1.2.10")
        XCTAssertNil(LinkAssessment.suggestedHostAddress(forPeer: "169.254.1.1"))
        XCTAssertNil(LinkAssessment.suggestedHostAddress(forPeer: "127.0.0.1"))
        XCTAssertNil(LinkAssessment.suggestedHostAddress(forPeer: "224.0.0.251"))
        XCTAssertNil(LinkAssessment.suggestedHostAddress(forPeer: "not an address"))
        XCTAssertNil(LinkAssessment.suggestedHostAddress(forPeer: "1.2.3"))
        XCTAssertNil(LinkAssessment.suggestedHostAddress(forPeer: "1.2.3.400"))
    }
}
