import Charts
import SwiftUI
import TetherKitNextIPC

/// Throughput and counters.
///
/// The chart deliberately draws only **rates** and not cumulative amounts: a cumulative curve rises monotonically forever and reveals nothing;
/// while a rate shows stalls, jitter and rate limiting at a glance. Cumulative amounts go in the metric cells below, readable when needed.
struct ThroughputCard: View {
    @Bindable var model: AppModel

    private var hasData: Bool { !model.throughputHistory.isEmpty }

    var body: some View {
        Card(title: L(.throughputSectionTitle), systemImage: "chart.line.uptrend.xyaxis", accessory: AnyView(liveBadge)) {
            VStack(alignment: .leading, spacing: Design.Spacing.medium) {
                rateRow
                chart
                Divider()
                counterRow
                if hasDrops {
                    dropHint
                }
            }
        }
    }

    @ViewBuilder
    private var liveBadge: some View {
        if model.status.runState == .running {
            StatusBadge(text: L(.throughputLive), color: .green)
        }
    }

    // The four rate cells are arranged 2x2 rather than in one row: this card now lives in a half column, and four cells in a row would squeeze
    // descriptions like "Download (device -> this machine)" into truncation.
    private var rateRow: some View {
        Grid(alignment: .leading,
             horizontalSpacing: Design.Spacing.medium,
             verticalSpacing: Design.Spacing.small) {
            GridRow {
                MetricTile(caption: L(.throughputDownstream),
                           value: Format.bitrate(model.throughput.receiveBitsPerSecond),
                           systemImage: "arrow.down",
                           tint: .blue)
                MetricTile(caption: L(.throughputUpstream),
                           value: Format.bitrate(model.throughput.transmitBitsPerSecond),
                           systemImage: "arrow.up",
                           tint: .purple)
            }
            GridRow {
                MetricTile(caption: L(.throughputDownstreamFPS),
                           value: "\(Format.packetsPerSecond(model.throughput.receivePacketsPerSecond)) pps",
                           systemImage: "square.stack.3d.up")
                MetricTile(caption: L(.throughputUpstreamFPS),
                           value: "\(Format.packetsPerSecond(model.throughput.transmitPacketsPerSecond)) pps",
                           systemImage: "square.stack.3d.up")
            }
        }
    }

    @ViewBuilder
    private var chart: some View {
        if hasData {
            Chart {
                ForEach(model.throughputHistory) { sample in
                    AreaMark(x: .value(L(.chartTime), sample.timestamp),
                             y: .value(L(.chartRate), sample.receiveBitsPerSecond))
                        .foregroundStyle(
                            .linearGradient(colors: [.blue.opacity(0.35), .blue.opacity(0.02)],
                                            startPoint: .top, endPoint: .bottom))
                        .interpolationMethod(.monotone)

                    LineMark(x: .value(L(.chartTime), sample.timestamp),
                             y: .value(L(.chartRate), sample.receiveBitsPerSecond),
                             series: .value(L(.chartDirection), L(.downstreamShort)))
                        .foregroundStyle(.blue)
                        .interpolationMethod(.monotone)

                    LineMark(x: .value(L(.chartTime), sample.timestamp),
                             y: .value(L(.chartRate), sample.transmitBitsPerSecond),
                             series: .value(L(.chartDirection), L(.upstreamShort)))
                        .foregroundStyle(.purple)
                        .interpolationMethod(.monotone)
                }
            }
            .chartYAxis {
                AxisMarks(position: .leading) { value in
                    AxisGridLine()
                    AxisValueLabel {
                        if let bits = value.as(Double.self) {
                            Text(Format.bitrate(bits)).font(.caption2)
                        }
                    }
                }
            }
            // The horizontal axis draws no ticks: this is a rolling "last 60 seconds" curve, specific moments are meaningless,
            // and drawing them would only take up the plotting area that is not tall to begin with.
            .chartXAxis(.hidden)
            .frame(height: 120)
        } else {
            RoundedRectangle(cornerRadius: Design.Radius.control)
                .fill(.quaternary.opacity(0.35))
                .frame(height: 120)
                .overlay(
                    Text(L(model.status.runState == .running ? .throughputCollecting : .throughputPlaceholder))
                        .font(.callout)
                        .foregroundStyle(.secondary))
        }
    }

    private var counterRow: some View {
        HStack(spacing: Design.Spacing.medium) {
            MetricTile(caption: L(.totalDownstream), value: Format.bytes(model.status.rxBytes))
            MetricTile(caption: L(.totalUpstream), value: Format.bytes(model.status.txBytes))
            MetricTile(caption: L(.downstreamFrames), value: Format.count(model.status.rxFrames))
            MetricTile(caption: L(.upstreamFrames), value: Format.count(model.status.txFrames))
        }
    }

    private var hasDrops: Bool {
        model.status.rxDropped > 0 || model.status.txDropped > 0
            || model.status.linkKernelDrops > 0 || model.status.txBackpressure > 0
    }

    /// Packet loss hint.
    ///
    /// The four numbers are listed separately rather than merged into one "loss rate": they point to completely different bottlenecks --
    /// queue full means the downstream cannot write fast enough, kernel drops mean we read too slowly, and backpressure means the USB side cannot send.
    /// After merging, one could no longer tell which parameter to tune.
    private var dropHint: some View {
        HStack(spacing: Design.Spacing.medium) {
            MetricTile(caption: L(.downstreamDropped), value: Format.count(model.status.rxDropped), tint: .orange)
            MetricTile(caption: L(.upstreamDropped), value: Format.count(model.status.txDropped), tint: .orange)
            MetricTile(caption: L(.kernelDrops), value: Format.count(model.status.linkKernelDrops), tint: .orange)
            MetricTile(caption: L(.transmitBackpressure), value: Format.count(model.status.txBackpressure), tint: .orange)
        }
    }
}
