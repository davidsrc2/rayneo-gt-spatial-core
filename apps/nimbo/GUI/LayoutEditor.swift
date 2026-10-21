import SwiftUI

// Monitors as cards on a grid. Drag a card onto a free cell (or another card to swap);
// it settles softly into place. Arrangements must stay connected, like real monitors.
struct LayoutEditor: View {
    @ObservedObject var model: AppModel
    @StateObject private var draggingBox = Box<Int?>(nil)
    @StateObject private var offsetBox = Box(CGSize.zero)
    private var dragging: Int? { draggingBox.value }
    private var dragOffset: CGSize { offsetBox.value }

    private let cell = CGSize(width: 132, height: 74)
    private let spacing: CGFloat = 10

    var body: some View {
        GeometryReader { geo in
            let n = model.normalizedCells
            let cols = (n.map(\.col).max() ?? 0) + 1
            let rows = (n.map(\.row).max() ?? 0) + 1
            let size = CGSize(width: CGFloat(cols) * (cell.width + spacing) - spacing,
                              height: CGFloat(rows) * (cell.height + spacing) - spacing)
            let origin = CGPoint(x: (geo.size.width - size.width) / 2, y: (geo.size.height - size.height) / 2)

            ZStack(alignment: .topLeading) {
                ForEach(Array(n.enumerated()), id: \.offset) { i, pos in
                    card(index: i)
                        .frame(width: cell.width, height: cell.height)
                        .position(x: origin.x + CGFloat(pos.col) * (cell.width + spacing) + cell.width / 2,
                                  y: origin.y + CGFloat(pos.row) * (cell.height + spacing) + cell.height / 2)
                        .offset(dragging == i ? dragOffset : .zero)
                        .zIndex(dragging == i ? 1 : 0)
                        .gesture(
                            DragGesture()
                                .onChanged { v in
                                    draggingBox.value = i
                                    offsetBox.value = v.translation
                                }
                                .onEnded { v in drop(index: i, translation: v.translation) }
                        )
                }
            }
            .animation(Theme.ease, value: model.cells)
        }
        .frame(height: 3 * (cell.height + spacing) + 30)
    }

    private func card(index i: Int) -> some View {
        RoundedRectangle(cornerRadius: 10, style: .continuous)
            .fill((i == 0 ? Theme.cardMain : Theme.card).opacity(dragging == i ? 0.95 : 0.7))
            .overlay(
                VStack(spacing: 2) {
                    Text("\(i + 1)").font(.system(size: 24, weight: .light)).foregroundStyle(Theme.text)
                    if i == 0 { Text("principal").font(Theme.small).foregroundStyle(Theme.text.opacity(0.7)) }
                }
            )
            .shadow(color: .black.opacity(dragging == i ? 0.35 : 0), radius: 10, y: 4)
    }

    // Snap to the nearest cell; swap with a card already there (core::move_cell). Rejected moves
    // glide back.
    private func drop(index i: Int, translation: CGSize) {
        let dc = Int((translation.width / (cell.width + spacing)).rounded())
        let dr = Int((translation.height / (cell.height + spacing)).rounded())
        withAnimation(Theme.ease) {
            draggingBox.value = nil
            offsetBox.value = .zero
            guard dc != 0 || dr != 0 else { return }
            let from = model.normalizedCells[i]
            model.moveCell(i, to: GridPos(col: from.col + dc, row: from.row + dr))
        }
    }
}

struct TemplateButton: View {
    let title: String
    let cells: [GridPos]
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 8) {
                MiniLayout(cells: cells).frame(width: 64, height: 36)
                Text(title).font(Theme.small).foregroundStyle(Theme.text)
            }
            .frame(width: 128, height: 82)
        }
        .buttonStyle(CalmButtonStyle())
    }
}

struct MiniLayout: View {
    let cells: [GridPos]

    var body: some View {
        GeometryReader { g in
            let cols = CGFloat((cells.map(\.col).max() ?? 0) + 1)
            let rows = CGFloat((cells.map(\.row).max() ?? 0) + 1)
            let w = min(g.size.width / cols, g.size.height * 16 / 9 / rows)
            let h = w * 9 / 16
            let ox = (g.size.width - w * cols) / 2, oy = (g.size.height - h * rows) / 2
            ForEach(Array(cells.enumerated()), id: \.offset) { _, c in
                RoundedRectangle(cornerRadius: 2)
                    .fill(Theme.card)
                    .frame(width: w - 3, height: h - 3)
                    .position(x: ox + (CGFloat(c.col) + 0.5) * w, y: oy + (CGFloat(c.row) + 0.5) * h)
            }
        }
    }
}
