import SwiftUI
import simd

// What the monitors look like from inside the glasses: perspective from the head, with a
// soft frame showing how much the glasses show at once. Same geometry as the renderer
// (core::grid_layout, flat panels tangent to the viewing circle, rows tilted by pitch).
struct LayoutPreview: View {
    @ObservedObject var model: AppModel

    private let previewHFov = 110.0 * Double.pi / 180  // wider than the glasses, to show the layout

    var body: some View {
        Canvas { ctx, size in
            ctx.fill(Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 14), with: .color(.black.opacity(0.35)))
            let f = (size.width / 2) / tan(previewHFov / 2)
            let center = CGPoint(x: size.width / 2, y: size.height / 2)
            func project(_ p: SIMD3<Double>) -> CGPoint? {
                guard p.z > 0.05 else { return nil }
                return CGPoint(x: center.x + f * p.x / p.z, y: center.y - f * p.y / p.z)
            }

            let size16x9 = AppModel.textSizes[max(0, min(AppModel.textSizes.count - 1, model.textSize))]
            let aspect = Double(size16x9.width) / Double(size16x9.height)
            let arc = model.size * .pi / 180, gap = model.gap * .pi / 180, r = model.distance
            let k = model.curvature
            let varc = 2 * atan(tan(arc / 2) / aspect)  // a bent sheet keeps the flat height
            let cells = model.normalizedCells
            let midC = Double((cells.map(\.col).max() ?? 0)) / 2, midR = Double((cells.map(\.row).max() ?? 0)) / 2
            let width = 2 * r * tan(arc / 2)
            let height = width / aspect

            for (i, c) in cells.enumerated() {
                let yaw = (Double(c.col) - midC) * (arc + gap)
                let pitch = (midR - Double(c.row)) * (varc + gap)
                let outline = panelOutline(yaw: yaw, pitch: pitch, r: r, width: width, height: height, curvature: k)
                let pts = outline.compactMap(project)
                guard pts.count == outline.count, let first = pts.first else { continue }
                var path = Path()
                path.move(to: first)
                pts.dropFirst().forEach { path.addLine(to: $0) }
                path.closeSubpath()
                ctx.fill(path, with: .color((i == 0 ? Theme.cardMain : Theme.card).opacity(0.55)))
                ctx.stroke(path, with: .color(Theme.text.opacity(0.35)), lineWidth: 1)
                if let mid = project(rotate(SIMD3(0, 0, r), yaw: yaw, pitch: pitch)) {
                    ctx.draw(Text("\(i + 1)").font(.system(size: 18, weight: .light)).foregroundColor(Theme.text), at: mid)
                }
            }

            // What the glasses show at once.
            let hv = tan(model.fov * .pi / 360) * f
            let hh = hv * 16 / 9
            let frame = CGRect(x: center.x - hh, y: center.y - hv, width: 2 * hh, height: 2 * hv)
            ctx.stroke(Path(roundedRect: frame, cornerRadius: 8), with: .color(Theme.accent.opacity(0.6)),
                       style: StrokeStyle(lineWidth: 1.5, dash: [6, 6]))
            ctx.draw(Text("lo que ves de una vez").font(Theme.small).foregroundColor(Theme.accent.opacity(0.8)),
                     at: CGPoint(x: center.x, y: frame.maxY + 14))
        }
        .frame(height: 220)
        .animation(Theme.ease, value: model.cells)
        .animation(Theme.ease, value: model.curvature)
    }

    private func rotate(_ p: SIMD3<Double>, yaw: Double, pitch: Double) -> SIMD3<Double> {
        // Yaw about render Y (positive = right), then tilt the row about X (positive = up).
        let x = p.x * cos(yaw) + p.z * sin(yaw), z = -p.x * sin(yaw) + p.z * cos(yaw)
        return SIMD3(x, p.y * cos(pitch) + z * sin(pitch), -p.y * sin(pitch) + z * cos(pitch))
    }

    // Same sheet bending as core::bent_screen_mesh.
    private func panelOutline(yaw: Double, pitch: Double, r: Double, width: Double, height: Double,
                              curvature k: Double) -> [SIMD3<Double>] {
        let steps = k > 0.001 ? 16 : 1
        var top: [SIMD3<Double>] = [], bottom: [SIMD3<Double>] = []
        for i in 0...steps {
            let u = Double(i) / Double(steps) - 0.5
            let local: SIMD3<Double>
            if k > 0.001 {
                let bend = r / min(k, 1), phi = u * width / bend
                local = SIMD3(bend * sin(phi), 0, (r - bend) + bend * cos(phi))
            } else {
                local = SIMD3(u * width, 0, r)
            }
            top.append(rotate(local + SIMD3(0, height / 2, 0), yaw: yaw, pitch: pitch))
            bottom.append(rotate(local - SIMD3(0, height / 2, 0), yaw: yaw, pitch: pitch))
        }
        return top + bottom.reversed()
    }
}
