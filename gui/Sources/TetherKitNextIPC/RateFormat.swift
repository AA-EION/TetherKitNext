import Foundation

/// Compact, fixed-width bit-rate text for the menu bar status item.
///
/// Upstream issue #1: the status item changed width as the rate moved between
/// "0K", "12K" and "120K", shoving every icon to its left around several times
/// a second. The output here is always exactly four characters — three for the
/// number (left-padded with FIGURE SPACE, which has digit width) plus a unit —
/// and is rendered in a monospaced font, so the width never changes.
///
///     0 → "  0K"   12_300 → " 12K"   9_870_000 → "9.9M"   123_000_000 → "123M"
public enum RateFormat {
    public static let figureSpace: Character = "\u{2007}"

    public static func compact(_ bitsPerSecond: Double) -> String {
        let value = bitsPerSecond.isFinite ? max(0, bitsPerSecond) : 0
        let units: [(divisor: Double, suffix: String)] = [
            (1_000, "K"), (1_000_000, "M"), (1_000_000_000, "G"), (1_000_000_000_000, "T"),
        ]
        for (index, unit) in units.enumerated() {
            let scaled = value / unit.divisor
            let isLast = index == units.count - 1
            // Move to the next unit before rounding would produce 4 digits.
            if scaled >= 999.5, !isLast { continue }
            let number: String
            if scaled < 9.95, unit.suffix != "K" {
                number = String(format: "%.1f", scaled)
            } else {
                number = String(format: "%.0f", min(scaled, 999))
            }
            return String(repeating: figureSpace, count: max(0, 3 - number.count))
                + number + unit.suffix
        }
        return "999T"
    }
}
