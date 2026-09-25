import XCTest
@testable import TetherKitIPC

/// Upstream issue #1: the menu bar item must not change width with the rate.
final class RateFormatTests: XCTestCase {
    func testAlwaysFourCharacters() {
        var value = 0.0
        while value < 5e12 {
            XCTAssertEqual(RateFormat.compact(value).count, 4, "value \(value)")
            value = value * 1.07 + 13
        }
        for edge in [0, 999, 999.4, 999.5, 9_949, 9_950, 999_499, 999_500, 9_949_999,
                     9_950_000, 99_500_000, 999_499_999, 999_500_000] as [Double] {
            XCTAssertEqual(RateFormat.compact(edge).count, 4, "value \(edge)")
        }
    }

    func testRepresentativeValues() {
        let pad = String(RateFormat.figureSpace)
        XCTAssertEqual(RateFormat.compact(0), pad + pad + "0K")
        XCTAssertEqual(RateFormat.compact(12_300), pad + "12K")
        XCTAssertEqual(RateFormat.compact(9_870_000), "9.9M")
        XCTAssertEqual(RateFormat.compact(123_000_000), "123M")
        XCTAssertEqual(RateFormat.compact(999_600), "1.0M")
        XCTAssertEqual(RateFormat.compact(1_500_000_000), "1.5G")
    }

    func testRejectsGarbage() {
        XCTAssertEqual(RateFormat.compact(-5).count, 4)
        XCTAssertEqual(RateFormat.compact(.nan).count, 4)
        XCTAssertEqual(RateFormat.compact(.infinity).count, 4)
    }
}
