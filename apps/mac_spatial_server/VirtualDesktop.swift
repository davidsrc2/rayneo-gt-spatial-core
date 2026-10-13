import AppKit
import CoreGraphics

// N virtual monitors laid out left -> right, mirroring the panels' physical arrangement so
// the cursor and dragged windows move between panels as the head does.
final class VirtualDesktop {
    private(set) var displays: [RNVirtualDisplay] = []
    var ids: [CGDirectDisplayID] { displays.map(\.displayID) }

    init(count: Int, width: Int, height: Int, hiDPI: Bool, refresh: Double) throws {
        for i in 0..<count {
            let d = try RNVirtualDisplay(name: "RayNeo Panel \(i + 1)", width: UInt(width), height: UInt(height),
                                         refreshRate: refresh, hiDPI: hiDPI, serial: UInt32(0x52_4E00 + i))
            displays.append(d)
        }
    }

    // App-scoped arrangement (reverted when the process exits, like the monitors themselves):
    // a row to the right of `anchor`, top-aligned.
    @discardableResult
    func arrange(rightOf anchor: CGDirectDisplayID) -> Bool {
        let a = CGDisplayBounds(anchor)
        var x = Int32(a.maxX)
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return false }
        for id in ids {
            CGConfigureDisplayOrigin(config, id, x, Int32(a.minY))
            x += Int32(CGDisplayBounds(id).width)
        }
        return CGCompleteDisplayConfiguration(config, .forAppOnly) == .success
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
