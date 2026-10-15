import AppKit
import CoreGraphics

// N virtual monitors laid out left -> right, mirroring the panels' physical arrangement so
// the cursor and dragged windows move between panels as the head does.
final class VirtualDesktop {
    private(set) var displays: [RNVirtualDisplay] = []
    var keep: [CGDirectDisplayID] = []  // left where they are when the rest is tucked away (floating windows' monitors)
    var ids: [CGDirectDisplayID] { displays.map(\.displayID) }

    init(count: Int, width: Int, height: Int, hiDPI: Bool, refresh: Double) throws {
        // macOS remembers the mode it last used per (vendor, product, serial). Encoding the
        // geometry in the serial gives every size/scale its own identity, so a remembered
        // 1920x1080 cannot override a 1280x720 HiDPI request.
        let geometry = UInt32(truncatingIfNeeded: (width / 16) << 16 | (height / 16) << 4) | (hiDPI ? 1 : 0) << 3
        for i in 0..<count {
            let d = try RNVirtualDisplay(name: "Nimbo \(i + 1)", width: UInt(width), height: UInt(height),
                                         refreshRate: refresh, hiDPI: hiDPI, serial: geometry | UInt32(i & 7))
            displays.append(d)
        }
    }

    // Origins (top-left, global coordinates) of the monitors laid out as `cells`, with
    // monitor 0 at `origin`. Same-size monitors: a cell is one monitor wide and tall.
    private func origins(cells: [GridPos], origin: CGPoint) -> [CGPoint] {
        let w = ids.first.map { CGDisplayBounds($0).width } ?? 0
        let h = ids.first.map { CGDisplayBounds($0).height } ?? 0
        let c0 = cells.first ?? GridPos(col: 0, row: 0)
        return cells.map { CGPoint(x: origin.x + CGFloat($0.col - c0.col) * w, y: origin.y + CGFloat($0.row - c0.row) * h) }
    }

    private func cellsOrRow(_ cells: [GridPos]?) -> [GridPos] {
        if let cells, cells.count == ids.count { return cells }
        return ids.indices.map { GridPos(col: $0, row: 0) }
    }

    // App-scoped arrangement (reverted when the process exits, like the monitors themselves):
    // the grid to the right of `anchor`, top-aligned.
    @discardableResult
    func arrange(rightOf anchor: CGDirectDisplayID, cells: [GridPos]? = nil) -> Bool {
        let grid = cellsOrRow(cells)
        let a = CGDisplayBounds(anchor)
        let minCol = grid.map(\.col).min() ?? 0, minRow = grid.map(\.row).min() ?? 0
        let w = ids.first.map { CGDisplayBounds($0).width } ?? 0
        let h = ids.first.map { CGDisplayBounds($0).height } ?? 0
        let first = grid[0]
        let o = CGPoint(x: a.maxX + CGFloat(first.col - minCol) * w, y: a.minY + CGFloat(first.row - minRow) * h)
        return apply(origins(cells: grid, origin: o), tucking: nil)
    }

    private func apply(_ points: [CGPoint], tucking physical: CGDirectDisplayID?) -> Bool {
        apply(Array(zip(ids, points)), tucking: physical)
    }

    private func apply(_ placed: [(CGDirectDisplayID, CGPoint)], tucking physical: CGDirectDisplayID?) -> Bool {
        let ids = placed.map(\.0), points = placed.map(\.1)
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return false }
        for (id, p) in placed { CGConfigureDisplayOrigin(config, id, Int32(p.x), Int32(p.y)) }
        if let physical {
            // Below-left of the row, touching it only at a corner: the glasses first, then every
            // other display (the Mac's own screen, externals) further left, touching only the one
            // before. Left where it was, the Mac's screen ended up between two monitors.
            let minX = points.map(\.x).min() ?? 0
            let maxY = zip(ids, points).map { $1.y + CGDisplayBounds($0).height }.max() ?? 0
            var x = minX
            for id in [physical] + Self.otherDisplays(excluding: ids + [physical] + keep) {
                x -= CGDisplayBounds(id).width
                CGConfigureDisplayOrigin(config, id, Int32(x), Int32(maxY))
            }
        }
        return CGCompleteDisplayConfiguration(config, .forAppOnly) == .success
    }

    // App-scoped: the virtual row becomes the desktop (the display at the origin is macOS's
    // main display: menu bar, Dock, new windows) and `physical` (the glasses, plus anything
    // mirroring it) is tucked below-left, touching the row only at a corner, so the cursor
    // practically never reaches the head-locked surface.
    // Grid cells map to monitor positions, so the cursor crosses between stacked monitors
    // vertically and between side-by-side ones horizontally, as with physical monitors.
    @discardableResult
    func arrangeAsMain(tucking physical: CGDirectDisplayID, cells: [GridPos]? = nil) -> Bool {
        apply(origins(cells: cellsOrRow(cells), origin: .zero), tucking: physical)
    }

    // App-scoped: `first` (the Mac's own screen) is panel 1 and the main display, at the origin;
    // the virtual monitors take the other cells (`cells` covers all panels, `first` included),
    // sized by their own monitors past it. The glasses and anything else are tucked away.
    @discardableResult
    func arrange(after first: CGDirectDisplayID, cells: [GridPos]?, tucking physical: CGDirectDisplayID) -> Bool {
        let n = ids.count + 1
        let grid = cells.flatMap { $0.count == n ? $0 : nil } ?? (0..<n).map { GridPos(col: $0, row: 0) }
        let f = CGDisplayBounds(first)
        let w = ids.first.map { CGDisplayBounds($0).width } ?? f.width
        let h = ids.first.map { CGDisplayBounds($0).height } ?? f.height
        // Cells past the first one start at its far edge; cells before it, one monitor each.
        let coord = { (d: Int, firstSize: CGFloat, size: CGFloat) -> CGFloat in
            d == 0 ? 0 : (d > 0 ? firstSize + CGFloat(d - 1) * size : CGFloat(d) * size)
        }
        let c0 = grid[0]
        var placed: [(CGDirectDisplayID, CGPoint)] = [(first, .zero)]
        for (id, c) in zip(ids, grid.dropFirst()) {
            placed.append((id, CGPoint(x: coord(c.col - c0.col, f.width, w), y: coord(c.row - c0.row, f.height, h))))
        }
        return apply(placed, tucking: physical)
    }

    // Active displays that are not these and not mirroring another (a mirror follows its source).
    private static func otherDisplays(excluding: [CGDirectDisplayID]) -> [CGDirectDisplayID] {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return [] }
        var list = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetActiveDisplayList(count, &list, &count) == .success else { return [] }
        return list.prefix(Int(count)).filter { !excluding.contains($0) && CGDisplayMirrorsDisplay($0) == kCGNullDirectDisplay }
    }

    func screen(for id: CGDirectDisplayID) -> NSScreen? {
        NSScreen.screens.first { Displays.id(of: $0) == id }
    }
}

// Big labelled grid per panel: shows layout, orientation and live updates without any
// user windows on the virtual monitors.
final class TestPatternView: NSView {
    private let index: Int
    private let total: Int
    private var timer: Timer?

    init(frame: NSRect, index: Int, total: Int) {
        self.index = index
        self.total = total
        super.init(frame: frame)
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.needsDisplay = true }
    }

    required init?(coder: NSCoder) { fatalError("unused") }

    override func draw(_ dirtyRect: NSRect) {
        let hues: [CGFloat] = [0.58, 0.33, 0.08, 0.83, 0.0, 0.5, 0.16, 0.7]
        NSColor(hue: hues[index % hues.count], saturation: 0.55, brightness: 0.35, alpha: 1).setFill()
        bounds.fill()

        NSColor(white: 1, alpha: 0.18).setStroke()
        let grid = NSBezierPath()
        stride(from: 0, through: bounds.width, by: 120).forEach { x in
            grid.move(to: NSPoint(x: x, y: 0)); grid.line(to: NSPoint(x: x, y: bounds.height))
        }
        stride(from: 0, through: bounds.height, by: 120).forEach { y in
            grid.move(to: NSPoint(x: 0, y: y)); grid.line(to: NSPoint(x: bounds.width, y: y))
        }
        grid.stroke()
        NSColor.white.setStroke()
        let border = NSBezierPath(rect: bounds.insetBy(dx: 6, dy: 6))
        border.lineWidth = 12
        border.stroke()

        let position = total == 1 ? "centre" : (index == 0 ? "left" : (index == total - 1 ? "right" : "centre"))
        let title = "PANEL \(index + 1) / \(total)"
        let sub = "\(position)  ·  \(Int(bounds.width))×\(Int(bounds.height))  ·  " +
            DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        draw(title, size: bounds.height * 0.14, y: bounds.midY + bounds.height * 0.02, weight: .bold)
        draw(sub, size: bounds.height * 0.045, y: bounds.midY - bounds.height * 0.08, weight: .regular)
        draw("↑ top", size: bounds.height * 0.04, y: bounds.maxY - bounds.height * 0.08, weight: .regular)
    }

    private func draw(_ s: String, size: CGFloat, y: CGFloat, weight: NSFont.Weight) {
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: size, weight: weight), .foregroundColor: NSColor.white,
        ]
        let str = NSAttributedString(string: s, attributes: attrs)
        let sz = str.size()
        str.draw(at: NSPoint(x: bounds.midX - sz.width / 2, y: y))
    }

    static func makeWindow(on screen: NSScreen, index: Int, total: Int) -> NSWindow {
        let w = NSWindow(contentRect: screen.visibleFrame, styleMask: [.borderless], backing: .buffered,
                         defer: false, screen: screen)
        w.contentView = TestPatternView(frame: NSRect(origin: .zero, size: screen.visibleFrame.size),
                                        index: index, total: total)
        w.level = .normal
        w.isReleasedWhenClosed = false
        w.collectionBehavior = [.stationary]
        w.setFrame(screen.visibleFrame, display: true)
        w.orderFrontRegardless()
        return w
    }
}
