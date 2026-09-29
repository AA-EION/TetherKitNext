import SwiftUI
import TetherKitNextIPC

/// The design language of the UI.
///
/// Centralizes spacing, corner radii, colors and a few reusable containers, with a single purpose: **make each visual decision
/// only once**. Magic numbers scattered across the various Views gradually drift out of shape as new panels are added, and no automated
/// means can detect this kind of drift.
enum Design {
    // MARK: - Sizes

    /// The spacing ladder. Advances in multiples of 4 -- from the same source as the internal padding of system controls, so mixing them does not misalign.
    enum Spacing {
        static let tight: CGFloat = 6
        static let small: CGFloat = 10
        /// The gutter between cards and between the two columns. The whole page must fit on one screen, and the card spacing is the first
        /// place to economize -- it occurs the most times and its information density is zero.
        static let gutter: CGFloat = 12
        static let medium: CGFloat = 16
        static let large: CGFloat = 24
        /// Card inner padding.
        static let section: CGFloat = 16
    }

    enum Radius {
        static let card: CGFloat = 18
        static let hero: CGFloat = 24
        static let control: CGFloat = 10
    }

    /// Window sizes. The window is a sidebar + one detail page, so the minimum
    /// only has to fit the widest single page (the static-IP form: two
    /// "label + 255.255.255.255" fields side by side) next to the sidebar.
    enum Window {
        static let minWidth: CGFloat = 820
        static let minHeight: CGFloat = 600
        static let defaultWidth: CGFloat = 1000
        static let defaultHeight: CGFloat = 720
        /// Content column cap: cards stay readable on wide windows.
        static let contentMaxWidth: CGFloat = 860
    }

    // MARK: - Status colors

    /// The accent color corresponding to the session state. The whole UI's color scheme is driven by it -- the user knows what the situation is
    /// at a glance of the color, without needing to read text.
    static func accent(for state: RunState) -> Color {
        switch state {
        case .idle, .stopped: return .secondary
        case .starting, .stopping: return .orange
        case .running: return .green
        case .failed: return .red
        }
    }

    static func statusLabel(for status: SessionStatus) -> String {
        switch status.runState {
        case .idle: return L(.statusDisconnected)
        case .starting: return L(.statusConnecting)
        case .running: return L(status.linkUp ? .statusConnected : .statusReadyLinkDown)
        case .stopping: return L(.statusDisconnecting)
        case .stopped: return L(.statusStopped)
        case .failed: return L(.statusFailed)
        }
    }

    /// The SF Symbol inside the status ring.
    static func statusSymbol(for status: SessionStatus) -> String {
        switch status.runState {
        case .idle, .stopped: return "bolt.horizontal"
        case .starting, .stopping: return "arrow.triangle.2.circlepath"
        case .running: return status.linkUp ? "bolt.horizontal.fill" : "bolt.horizontal"
        case .failed: return "exclamationmark.triangle.fill"
        }
    }

    static func logColor(for level: LogLevel) -> Color {
        switch level {
        case .trace, .debug: return .secondary
        case .info: return .primary
        case .warning: return .orange
        case .error: return .red
        }
    }
}

// MARK: - Reusable containers

/// The card container. All content panels wrap themselves in it, guaranteeing consistent corner radii, padding and strokes.
struct Card<Content: View>: View {
    var title: String?
    var systemImage: String?
    /// The auxiliary view to the right of the title (refresh button, status badge, etc.).
    var accessory: AnyView?
    @ViewBuilder var content: () -> Content

    init(title: String? = nil,
         systemImage: String? = nil,
         accessory: AnyView? = nil,
         @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.systemImage = systemImage
        self.accessory = accessory
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Design.Spacing.medium) {
            if let title {
                HStack(spacing: Design.Spacing.tight) {
                    if let systemImage {
                        Image(systemName: systemImage)
                            .foregroundStyle(.secondary)
                    }
                    Text(title)
                        .font(.headline)
                    Spacer(minLength: Design.Spacing.small)
                    accessory
                }
            }
            content()
        }
        .padding(Design.Spacing.section)
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentSurface()
    }
}

// MARK: - Liquid Glass
//
// Apple's guidance for Liquid Glass (macOS 26): glass belongs to the
// *navigation and control* layer that floats above content — sidebars,
// toolbars, the primary action, status chrome — not to every content card.
// So the sidebar and toolbar get it from the system automatically; the status
// hero, primary buttons and menu bar controls use it explicitly; content
// cards stay on a quiet material surface that the glass refracts.
//
// Every API is gated twice: `#if compiler(>=6.2)` so the package still builds
// with an older toolchain, and `#available(macOS 26, *)` so the app runs on
// macOS 14/15 with the material look.

extension View {
    /// Quiet surface for content cards.
    func contentSurface(cornerRadius: CGFloat = Design.Radius.card) -> some View {
        background(.regularMaterial,
                   in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .strokeBorder(.separator.opacity(0.5), lineWidth: 0.5))
    }

    /// Liquid Glass on macOS 26+, a tinted material elsewhere.
    @ViewBuilder
    func glassSurface(cornerRadius: CGFloat = Design.Radius.card, tint: Color? = nil,
                      interactive: Bool = false) -> some View {
        #if compiler(>=6.2)
        if #available(macOS 26.0, *) {
            glassEffect(Design.glass(tint: tint, interactive: interactive),
                        in: .rect(cornerRadius: cornerRadius, style: .continuous))
        } else {
            materialGlassFallback(cornerRadius: cornerRadius, tint: tint)
        }
        #else
        materialGlassFallback(cornerRadius: cornerRadius, tint: tint)
        #endif
    }

    /// Capsule-shaped glass for small chips (status badges, menu bar speeds).
    @ViewBuilder
    func glassCapsule(tint: Color? = nil) -> some View {
        #if compiler(>=6.2)
        if #available(macOS 26.0, *) {
            glassEffect(Design.glass(tint: tint, interactive: false), in: .capsule)
        } else {
            background((tint ?? .secondary).opacity(0.14), in: Capsule())
        }
        #else
        background((tint ?? .secondary).opacity(0.14), in: Capsule())
        #endif
    }

    /// The primary call to action: `.glassProminent` on macOS 26.
    @ViewBuilder
    func primaryActionButtonStyle() -> some View {
        #if compiler(>=6.2)
        if #available(macOS 26.0, *) {
            buttonStyle(.glassProminent)
        } else {
            buttonStyle(.borderedProminent)
        }
        #else
        buttonStyle(.borderedProminent)
        #endif
    }

    /// Secondary actions: `.glass` on macOS 26.
    @ViewBuilder
    func secondaryActionButtonStyle() -> some View {
        #if compiler(>=6.2)
        if #available(macOS 26.0, *) {
            buttonStyle(.glass)
        } else {
            buttonStyle(.bordered)
        }
        #else
        buttonStyle(.bordered)
        #endif
    }

    /// Groups adjacent glass shapes so they blend and morph together.
    @ViewBuilder
    func glassGroup(spacing: CGFloat = Design.Spacing.small) -> some View {
        #if compiler(>=6.2)
        if #available(macOS 26.0, *) {
            GlassEffectContainer(spacing: spacing) { self }
        } else {
            self
        }
        #else
        self
        #endif
    }

    private func materialGlassFallback(cornerRadius: CGFloat, tint: Color?) -> some View {
        background(.thinMaterial,
                   in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
            .background((tint ?? .clear).opacity(0.10),
                        in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .strokeBorder(.white.opacity(0.18), lineWidth: 0.5))
    }
}

#if compiler(>=6.2)
extension Design {
    @available(macOS 26.0, *)
    static func glass(tint: Color?, interactive: Bool) -> Glass {
        var glass = Glass.regular
        if let tint { glass = glass.tint(tint.opacity(0.35)) }
        if interactive { glass = glass.interactive() }
        return glass
    }
}
#endif

/// One metric cell: the description on top, the value below.
struct MetricTile: View {
    let caption: String
    let value: String
    var systemImage: String?
    var tint: Color = .primary

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 4) {
                if let systemImage {
                    Image(systemName: systemImage)
                        .font(.caption2)
                }
                Text(caption)
                    .font(.caption)
            }
            .foregroundStyle(.secondary)

            Text(value)
                // Monospaced digits: characters do not jitter left and right when values change.
                .font(.system(.title3, design: .rounded).weight(.medium))
                .monospacedDigit()
                .foregroundStyle(tint)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A small dot badge, for binary states such as "link connected / disconnected".
struct StatusBadge: View {
    let text: String
    let color: Color

    var body: some View {
        HStack(spacing: 5) {
            Circle()
                .fill(color)
                .frame(width: 7, height: 7)
            Text(text)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, Design.Spacing.small)
        .padding(.vertical, 4)
        .glassCapsule(tint: color)
    }
}

// MARK: - Formatting

/// The formatting of all numeric values on the UI goes through here, avoiding the same quantity being displayed looking different in different panels.
enum Format {
    /// Rate. Automatically picks the unit among bit/s, Kbps, Mbps, Gbps.
    ///
    /// Bits rather than bytes: the industry convention for network throughput is bits, consistent with the figures the user sees on routers and
    /// at carriers.
    static func bitrate(_ bitsPerSecond: Double) -> String {
        let units: [(threshold: Double, suffix: String, divisor: Double)] = [
            (1_000_000_000, "Gbps", 1_000_000_000),
            (1_000_000, "Mbps", 1_000_000),
            (1_000, "Kbps", 1_000),
        ]
        for unit in units where bitsPerSecond >= unit.threshold {
            return String(format: "%.1f %@", bitsPerSecond / unit.divisor, unit.suffix)
        }
        return String(format: "%.0f bps", max(0, bitsPerSecond))
    }

    /// Cumulative byte count.
    static func bytes(_ value: UInt64) -> String {
        // ByteCountFormatter renders 0 as "Zero KB" (its localized behavior),
        // which looks especially abrupt in a row of numbers, so it is handled separately.
        guard value > 0 else { return "0 B" }
        let formatter = ByteCountFormatter()
        formatter.countStyle = .binary
        return formatter.string(fromByteCount: Int64(clamping: value))
    }

    /// Adds thousands separators to large integers, easier to read as "12,345,678 frames".
    static func count(_ value: UInt64) -> String {
        let formatter = NumberFormatter()
        formatter.numberStyle = .decimal
        return formatter.string(from: NSNumber(value: value)) ?? "\(value)"
    }

    /// Fixed-width rate for the menu bar (upstream issue #1): always three
    /// significant characters plus a unit, so the status item does not jump
    /// around as the rate changes between 1K and 100K.
    static func compactBitrate(_ bitsPerSecond: Double) -> String {
        RateFormat.compact(bitsPerSecond)
    }

    static func packetsPerSecond(_ value: Double) -> String {
        value >= 10_000
            ? String(format: "%.1f k", value / 1000)
            : String(format: "%.0f", max(0, value))
    }

    // DateFormatter is not Sendable, but formatting with a fixed format on an
    // unmutated instance is thread-safe on every supported macOS.
    nonisolated(unsafe) private static let timeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter
    }()

    static func time(_ date: Date) -> String {
        timeFormatter.string(from: date)
    }

    /// Renders seconds as "1:02:03" / "2:03".
    static func duration(_ seconds: TimeInterval) -> String {
        let total = Int(max(0, seconds))
        let hours = total / 3600
        let minutes = (total % 3600) / 60
        let secs = total % 60
        return hours > 0
            ? String(format: "%d:%02d:%02d", hours, minutes, secs)
            : String(format: "%d:%02d", minutes, secs)
    }
}
