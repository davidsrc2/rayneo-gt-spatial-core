import SwiftUI

// Continuous slider that settles on the nearest anchor when released close to it, with the
// anchors shown as small labelled ticks. Precise when you want it, tidy by default.
struct SnapSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let anchors: [(Double, String)]
    var format: (Double) -> String = { String(format: "%.1f", $0) }
    var onCommit: () -> Void = {}

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(title).font(.system(size: 14, weight: .medium))
                Spacer()
                Text(format(value)).font(.system(size: 13)).foregroundStyle(.secondary).monospacedDigit()
            }
            Slider(value: $value, in: range) { editing in
                if !editing {
                    snap()
                    onCommit()
                }
            }
            .controlSize(.small)
            .tint(Color(red: 0.55, green: 0.72, blue: 0.70))
            GeometryReader { g in
                ForEach(Array(anchors.enumerated()), id: \.offset) { i, a in
                    let x = (a.0 - range.lowerBound) / (range.upperBound - range.lowerBound) * (g.size.width - 16) + 8
                    // End labels lean inwards so they never leave the panel.
                    let align: HorizontalAlignment = x < 30 ? .leading : (x > g.size.width - 30 ? .trailing : .center)
                    let labelWidth: CGFloat = 90
                    let cx = align == .leading ? x - 1.5 + labelWidth / 2 : (align == .trailing ? x + 1.5 - labelWidth / 2 : x)
                    VStack(alignment: align, spacing: 2) {
                        Circle().fill(.secondary.opacity(abs(value - a.0) < 1e-6 ? 0.9 : 0.35)).frame(width: 3, height: 3)
                        Text(a.1).font(.system(size: 10)).foregroundStyle(.secondary)
                    }
                    .frame(width: labelWidth, alignment: Alignment(horizontal: align, vertical: .center))
                    .position(x: cx, y: 8)
                    .onTapGesture {
                        withAnimation(.easeInOut(duration: 0.3)) { value = a.0 }
                        onCommit()
                    }
                }
            }
            .frame(height: 18)
        }
    }

    private func snap() {
        let span = range.upperBound - range.lowerBound
        guard let nearest = anchors.min(by: { abs($0.0 - value) < abs($1.0 - value) }),
              abs(nearest.0 - value) <= span * 0.06 else { return }
        withAnimation(.easeInOut(duration: 0.3)) { value = nearest.0 }
    }
}
