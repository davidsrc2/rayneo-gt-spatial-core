import SwiftUI

// One-click layouts drawn as what you get: monitors seen from above, around your head.
struct LayoutCards: View {
    @ObservedObject var model: AppModel
    let onChange: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            ForEach(AppModel.layouts) { preset in
                let selected = model.activeLayout == preset.id
                Button {
                    withAnimation(.easeInOut(duration: 0.35)) { model.apply(preset) }
                    onChange()
                } label: {
                    VStack(spacing: 10) {
                        LayoutGlyph(count: preset.cells.count, curvature: preset.curvature)
                            .frame(height: 30)
                        Text(preset.title)
                            .font(.system(size: 12, weight: selected ? .semibold : .regular))
                            .multilineTextAlignment(.center)
                            .lineLimit(2)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .frame(maxWidth: .infinity, minHeight: 84)
                    .padding(.vertical, 6)
                    .background(RoundedRectangle(cornerRadius: 14, style: .continuous)
                        .fill(.white.opacity(selected ? 0.14 : 0.05)))
                    .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous)
                        .stroke(.white.opacity(selected ? 0.35 : 0.08), lineWidth: selected ? 1.2 : 1))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }
}

// Top view: an arc of screens in front of a small head.
struct LayoutGlyph: View {
    let count: Int
    let curvature: Double

    var body: some View {
        Canvas { ctx, size in
            let head = CGPoint(x: size.width / 2, y: size.height - 3)
            ctx.fill(Path(ellipseIn: CGRect(x: head.x - 3, y: head.y - 3, width: 6, height: 6)), with: .color(.secondary))
            let r = size.height - 6
            let each = (count == 1 ? 70.0 : 40.0) * .pi / 180
            let total = each * Double(count)
            for i in 0..<count {
                let a0 = -total / 2 + Double(i) * each + 0.04, a1 = a0 + each - 0.08
                var p = Path()
                let steps = curvature > 0.2 ? 8 : 1
                for s in 0...steps {
                    let t = a0 + (a1 - a0) * Double(s) / Double(steps)
                    // Flat screens: straight chord; curved: along the arc.
                    let rr = steps == 1 ? r : r
                    let pt = CGPoint(x: head.x + rr * sin(t), y: head.y - rr * cos(t))
                    s == 0 ? p.move(to: pt) : p.addLine(to: pt)
                }
                ctx.stroke(p, with: .color(.primary.opacity(0.85)), style: StrokeStyle(lineWidth: 3, lineCap: .round))
            }
        }
    }
}

struct ProfileChips: View {
    @ObservedObject var model: AppModel
    let onChange: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            ForEach(AppModel.profiles) { p in
                let active = model.activeProfile == p.id
                Button {
                    withAnimation(.easeInOut(duration: 0.35)) { model.apply(p) }
                    onChange()
                } label: {
                    Label("Modo \(p.title)", systemImage: p.symbol)
                        .font(.system(size: 12, weight: active ? .semibold : .regular))
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Capsule().fill(.white.opacity(active ? 0.16 : 0.06)))
                        .overlay(Capsule().stroke(.white.opacity(active ? 0.3 : 0.08)))
                }
                .buttonStyle(.plain)
            }
        }
    }
}
