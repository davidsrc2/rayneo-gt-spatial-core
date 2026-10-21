import AppKit
import SwiftUI

// Hand placement of the monitors and floating windows, seen from where you sit: drag each one
// where you want it (it moves on the glasses at the same time), pick one to change its own
// size. The card follows the pointer; an outline shows where it will land. Edges and centres
// snap to the others (hold ⌥ to place freely), and a drop over another monitor glides back.
// On the wall (Pared continua) the monitors are one block: a monitor dropped on another cell
// moves there (swapping with the one there), never loose in the space.
struct SpatialEditor: View {
    @ObservedObject var model: AppModel
    @StateObject private var selectedBox = Box(0)
    @StateObject private var dragStartBox = Box<[PanelPos]?>(nil)
    @StateObject private var startPlacedBox = Box<[PanelPos]>([])  // model.panelPoses when the drag began
    // The card under the pointer (index, where the pointer has it, unsnapped).
    @StateObject private var draggedBox = Box<DraggedCard?>(nil)
    private var selected: Int { selectedBox.value }

    struct DraggedCard {
        let index: Int
        let pose: PanelPos
        let target: PanelPos?   // where it lands (nil: nowhere, the drop is undone)
    }

    private let canvas = CGSize(width: 640, height: 280)
    private let spanDeg: Float = 150  // horizontal degrees shown across the canvas
    private var ppd: CGFloat { canvas.width / CGFloat(spanDeg) }  // points per degree
    private let snapDeg: Float = 1.5

    private var monitorCount: Int { model.effectivePoses.count }
    // Floating windows the server has placed (pose known), with their index in model.floating.
    private var floatIndices: [Int] { model.floating.indices.filter { model.floating[$0].pose != nil } }
    // Monitors first, then floating windows: one list for drawing, snapping and sizing.
    private var items: [PanelPos] { model.effectivePoses + floatIndices.compactMap { model.floating[$0].pose } }
    // The wall's monitors move between cells; floating windows are always free.
    private func onWall(_ i: Int) -> Bool { model.wallActive && i < monitorCount }

    private func label(_ i: Int) -> String { i < monitorCount ? "\(i + 1)" : "V\(i - monitorCount + 1)" }

    var body: some View {
        let poses = items
        VStack(alignment: .leading, spacing: 12) {
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color.black.opacity(0.35))
                // What the glasses show when you look straight ahead.
                let view = viewFrameDeg
                RoundedRectangle(cornerRadius: 6)
                    .strokeBorder(style: StrokeStyle(lineWidth: 1, dash: [5, 4]))
                    .foregroundStyle(Theme.secondary.opacity(0.6))
                    .frame(width: CGFloat(view.width) * ppd, height: CGFloat(view.height) * ppd)
                    .position(point(yaw: 0, pitch: 0))
                Text("lo que ves mirando al frente")
                    .font(Theme.small).foregroundStyle(Theme.secondary.opacity(0.7))
                    .position(x: point(yaw: 0, pitch: 0).x, y: point(yaw: 0, pitch: 0).y + CGFloat(view.height) * ppd / 2 + 10)
                if let d = draggedBox.value { landing(d) }
                ForEach(Array(poses.enumerated()), id: \.offset) { i, p in
                    monitor(index: i, pose: draggedBox.value?.index == i ? draggedBox.value!.pose : p)
                }
            }
            .frame(width: canvas.width, height: canvas.height)
            .clipped()

            HStack(spacing: 14) {
                Text(selected < monitorCount ? "Monitor \(selected + 1)" : "Ventana \(label(selected))")
                    .font(Theme.body).foregroundStyle(Theme.text).frame(width: 110, alignment: .leading)
                Slider(value: Binding(get: { Double(poses[safe: selected]?.scale ?? 1) }, set: { setScale(Float($0)) }),
                       in: 0.5...1.6, step: 0.05)
                    .tint(Theme.accent)
                    .disabled(onWall(selected))
                Text(String(format: "%.0f %%", (poses[safe: selected]?.scale ?? 1) * 100))
                    .font(Theme.label).foregroundStyle(Theme.secondary).frame(width: 54, alignment: .trailing)
                Button("Volver a la cuadrícula") { withAnimation(Theme.ease) { model.resetPlacement() } }
                    .buttonStyle(CalmButtonStyle())
                    .disabled(model.panelPoses.isEmpty || model.wallActive)
            }
            Text(model.wallActive
                 ? "Pared continua: los monitores forman un bloque. Arrastra uno a otra casilla (o sobre otro para intercambiarlos); las ventanas flotantes se colocan libres."
                 : "Arrastra cada monitor a su sitio; se mueve en las gafas a la vez. Se alinea solo con los demás (mantén ⌥ para colocarlo libre). Elige uno para cambiar su tamaño.")
                .font(Theme.small).foregroundStyle(Theme.secondary)
        }
    }

    // MARK: drawing

    private func size(of p: PanelPos) -> CGSize {
        CGSize(width: CGFloat(Float(model.size) * p.scale) * ppd, height: CGFloat(AppModel.verticalArc(Float(model.size) * p.scale)) * ppd)
    }

    private func monitor(index i: Int, pose p: PanelPos) -> some View {
        let s = size(of: p)
        let isWindow = i >= monitorCount
        let dragged = draggedBox.value?.index == i
        return RoundedRectangle(cornerRadius: model.wallActive && !isWindow ? 2 : 6, style: .continuous)
            .fill((isWindow ? Theme.accent.opacity(0.55) : i == 0 ? Theme.cardMain : Theme.card).opacity(i == selected ? 0.95 : 0.7))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.accent.opacity(i == selected ? 0.9 : 0), lineWidth: 1.5))
            .overlay(Text(label(i)).font(.system(size: 20, weight: .light)).foregroundStyle(Theme.text))
            .frame(width: s.width - (model.wallActive && !isWindow ? 1 : 0), height: s.height - (model.wallActive && !isWindow ? 1 : 0))
            .shadow(color: .black.opacity(dragged ? 0.35 : 0), radius: 10, y: 4)
            .zIndex(dragged ? 1 : 0)
            .position(point(yaw: p.yaw, pitch: p.pitch))
            .gesture(
                DragGesture(minimumDistance: 1)
                    .onChanged { v in drag(index: i, translation: v.translation) }
                    .onEnded { _ in drop(index: i) }
            )
            .onTapGesture { selectedBox.value = i }
    }

    // Where the dragged card will land: an outline there, grey when the drop would be undone.
    @ViewBuilder
    private func landing(_ d: DraggedCard) -> some View {
        let p = d.target ?? dragStartBox.value?[safe: d.index] ?? d.pose
        let s = size(of: p)
        RoundedRectangle(cornerRadius: 6)
            .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6, 4]))
            .foregroundStyle(d.target != nil ? Theme.accent : Theme.secondary.opacity(0.7))
            .background(RoundedRectangle(cornerRadius: 6).fill((d.target != nil ? Theme.accent : Theme.secondary).opacity(0.12)))
            .frame(width: s.width, height: s.height)
            .position(point(yaw: p.yaw, pitch: p.pitch))
    }

    private func point(yaw: Float, pitch: Float) -> CGPoint {
        CGPoint(x: canvas.width / 2 + CGFloat(yaw) * ppd, y: canvas.height / 2 - CGFloat(pitch) * ppd)
    }

    // Degrees the glasses show around straight ahead, in the editor's units (panel size at 2 m).
    private var viewFrameDeg: (width: Float, height: Float) {
        let fovY = Float(model.fov)
        let hfov = 2 * atan(tan(fovY * .pi / 360) * AppModel.eyeAspect) * 180 / .pi
        return (hfov, fovY)
    }

    // MARK: editing

    // The pose is always the start pose plus the pointer's whole translation (never added up
    // step by step), so the card stays under the pointer.
    private func drag(index i: Int, translation: CGSize) {
        selectedBox.value = i
        if dragStartBox.value == nil {
            dragStartBox.value = items
            startPlacedBox.value = model.panelPoses
        }
        guard var poses = dragStartBox.value, poses.indices.contains(i) else { return }
        var p = poses[i]
        p.yaw += Float(translation.width / ppd)
        p.pitch -= Float(translation.height / ppd)
        if onWall(i) {
            let cell = wallCell(at: p)
            let ok = cell.map { model.moveCell(i, to: $0, apply: false) } ?? false
            draggedBox.value = DraggedCard(index: i, pose: p, target: ok ? wallPose(of: cell!) : nil)
            return
        }
        let snapped = NSEvent.modifierFlags.contains(.option) ? p : snap(p, index: i, among: poses)
        poses[i] = snapped
        let fits = i >= monitorCount || !model.poseOverlaps(Array(poses.prefix(monitorCount)), i)
        draggedBox.value = DraggedCard(index: i, pose: p, target: fits ? snapped : nil)
        if fits { write(poses, changed: i, settle: false) }  // live on the glasses
    }

    // Lands where the outline was; with no outline (over another monitor, or a cell that would
    // split the wall) everything glides back to where it was.
    private func drop(index i: Int) {
        guard let d = draggedBox.value, let start = dragStartBox.value else { return }
        withAnimation(Theme.ease) {
            draggedBox.value = nil
            dragStartBox.value = nil
            if onWall(i) {
                if d.target != nil, let cell = wallCell(at: d.pose) { model.moveCell(i, to: cell) }
                return
            }
            var poses = start
            if let t = d.target { poses[i] = t }
            if i < monitorCount {
                // The OS arrangement follows the monitors' new order, once.
                if d.target != nil { model.place(Array(poses.prefix(monitorCount))) } else { model.panelPoses = startPlacedBox.value }
            } else {
                write(poses, changed: i, settle: false)
            }
        }
    }

    // The wall's cell (normalised grid) whose place is nearest a pose; may lie beyond the block.
    private func wallCell(at p: PanelPos) -> GridPos? {
        let n = model.normalizedCells
        guard !n.isEmpty else { return nil }
        let (midC, midR) = wallMid(n)
        let w = Float(model.size), h = AppModel.verticalArc(Float(model.size))
        return GridPos(col: Int((p.yaw / w + midC).rounded()), row: Int((midR - p.pitch / h).rounded()))
    }

    private func wallPose(of c: GridPos) -> PanelPos {
        let (midC, midR) = wallMid(model.normalizedCells)
        let w = Float(model.size), h = AppModel.verticalArc(Float(model.size))
        return PanelPos(yaw: (Float(c.col) - midC) * w, pitch: (midR - Float(c.row)) * h, scale: 1)
    }

    private func wallMid(_ n: [GridPos]) -> (Float, Float) {
        let cols = n.map(\.col), rows = n.map(\.row)
        return (Float((cols.min() ?? 0) + (cols.max() ?? 0)) / 2, Float((rows.min() ?? 0) + (rows.max() ?? 0)) / 2)
    }

    private func setScale(_ s: Float) {
        guard !onWall(selected) else { return }
        var poses = items
        guard poses.indices.contains(selected) else { return }
        poses[selected].scale = s
        write(poses, changed: selected, settle: true)
    }

    // Monitors back to panelPoses (settle: the OS arrangement follows), windows to model.floating.
    private func write(_ poses: [PanelPos], changed i: Int, settle: Bool) {
        let m = monitorCount
        if i < m {
            let monitors = Array(poses.prefix(m))
            if settle { model.place(monitors) } else { model.panelPoses = monitors }
        } else if floatIndices.indices.contains(i - m) {
            model.floating[floatIndices[i - m]].pose = poses[i]
        }
    }

    // Edges touching, centres and tops/bottoms aligned with the other monitors, and the
    // horizon, within snapDeg.
    private func snap(_ p: PanelPos, index i: Int, among poses: [PanelPos]) -> PanelPos {
        var out = p
        let size = Float(model.size)
        let halfW = size * p.scale / 2, halfH = AppModel.verticalArc(size * p.scale) / 2
        var bestX = snapDeg, bestY = snapDeg
        var xTargets: [Float] = [0], yTargets: [Float] = [0]
        for (j, o) in poses.enumerated() where j != i {
            let oW = size * o.scale / 2, oH = AppModel.verticalArc(size * o.scale) / 2
            let g = Float(model.gap)
            xTargets += [o.yaw, o.yaw + oW + g + halfW, o.yaw - oW - g - halfW]  // centre, right of, left of
            yTargets += [o.pitch, o.pitch + oH - halfH, o.pitch - oH + halfH,       // centre, tops, bottoms
                         o.pitch + oH + g + halfH, o.pitch - oH - g - halfH]       // above, below
        }
        for t in xTargets where abs(p.yaw - t) < bestX {
            bestX = abs(p.yaw - t)
            out.yaw = t
        }
        for t in yTargets where abs(p.pitch - t) < bestY {
            bestY = abs(p.pitch - t)
            out.pitch = t
        }
        return out
    }
}

extension Array {
    subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil }
}
