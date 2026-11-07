import XCTest

@testable import TetherKitNextUIModel

final class InterfaceTrafficTests: XCTestCase {

    private func snapshot(_ name: String, rx: UInt64, tx: UInt64) -> InterfaceSnapshot {
        InterfaceSnapshot(name: name, isUp: true, isRunning: true, isLoopback: false,
                          linkAddress: nil, mtu: 1500, receivedBytes: rx, sentBytes: tx,
                          ipv4Addresses: ["192.0.2.1"], ipv6Addresses: [])
    }

    func testFirstSampleReportsZero() {
        var tracker = InterfaceRateTracker()
        let rates = tracker.update(with: [snapshot("en0", rx: 1_000, tx: 2_000)], at: 10)
        XCTAssertEqual(rates["en0"], .zero)
    }

    func testRateIsBitsPerSecond() {
        var tracker = InterfaceRateTracker()
        _ = tracker.update(with: [snapshot("en0", rx: 0, tx: 0)], at: 10)
        let rates = tracker.update(with: [snapshot("en0", rx: 125_000, tx: 250_000)], at: 11)
        XCTAssertEqual(rates["en0"]?.receiveBitsPerSecond ?? -1, 1_000_000, accuracy: 0.001)
        XCTAssertEqual(rates["en0"]?.transmitBitsPerSecond ?? -1, 2_000_000, accuracy: 0.001)
    }

    func testRateAcrossHalfSecondInterval() {
        var tracker = InterfaceRateTracker()
        _ = tracker.update(with: [snapshot("en0", rx: 0, tx: 0)], at: 0)
        let rates = tracker.update(with: [snapshot("en0", rx: 1_000, tx: 0)], at: 0.5)
        XCTAssertEqual(rates["en0"]?.receiveBitsPerSecond ?? -1, 16_000, accuracy: 0.001)
    }

    /// A recreated interface (same name, counters back near 0) must not
    /// report a spike; the direction that did not go backwards still works.
    func testCounterResetReportsZero() {
        var tracker = InterfaceRateTracker()
        _ = tracker.update(with: [snapshot("feth0", rx: 3_000_000_000, tx: 10)], at: 0)
        let rates = tracker.update(with: [snapshot("feth0", rx: 100, tx: 20)], at: 0.1)
        XCTAssertEqual(rates["feth0"]?.receiveBitsPerSecond, 0)
        XCTAssertEqual(rates["feth0"]?.transmitBitsPerSecond ?? -1, 800, accuracy: 0.001)
    }

    /// Counters beyond 4 GiB (the old 32-bit if_data limit) keep working.
    func testLargeCountersDoNotWrap() {
        var tracker = InterfaceRateTracker()
        let base: UInt64 = 5_000_000_000
        _ = tracker.update(with: [snapshot("en0", rx: base, tx: 0)], at: 0)
        let rates = tracker.update(with: [snapshot("en0", rx: base + 1_250_000, tx: 0)], at: 1)
        XCTAssertEqual(rates["en0"]?.receiveBitsPerSecond ?? -1, 10_000_000, accuracy: 0.001)
    }

    func testVanishedInterfaceStartsFreshWhenItReturns() {
        var tracker = InterfaceRateTracker()
        _ = tracker.update(with: [snapshot("en5", rx: 0, tx: 0)], at: 0)
        _ = tracker.update(with: [], at: 1)
        let rates = tracker.update(with: [snapshot("en5", rx: 50_000, tx: 0)], at: 2)
        XCTAssertEqual(rates["en5"], .zero)
    }

    func testNonIncreasingTimeReportsZero() {
        var tracker = InterfaceRateTracker()
        _ = tracker.update(with: [snapshot("en0", rx: 0, tx: 0)], at: 5)
        let rates = tracker.update(with: [snapshot("en0", rx: 9_000, tx: 0)], at: 5)
        XCTAssertEqual(rates["en0"], .zero)
    }

    /// Smoke test against the real system: every Mac has a loopback.
    func testSamplerFindsLoopback() {
        let interfaces = InterfaceSampler.snapshot()
        let loopback = interfaces.first { $0.name == "lo0" }
        XCTAssertNotNil(loopback)
        XCTAssertEqual(loopback?.isLoopback, true)
        XCTAssertTrue(loopback?.ipv4Addresses.contains("127.0.0.1") ?? false)
        XCTAssertEqual(Set(interfaces.map(\.name)).count, interfaces.count, "one entry per name")
    }

    /// The 64-bit counter dump parses and names the loopback interface.
    func testCounterDumpIncludesLoopback() {
        XCTAssertNotNil(InterfaceSampler.counters64()["lo0"])
    }
}
