// Measurement load: on every Nimbo virtual monitor ("Nimbo N"), a full-screen window that
// changes every frame at 60 fps (a sweeping bar and a frame counter), so ScreenCaptureKit
// delivers 60 frames per source per second, the same in every session. With the monitors
// static, captures drop to a few frames per second and missed refreshes fall with them: runs
// could not be compared.
//
//   swiftc -O scripts/capture_load.swift -o build/capture_load
//   build/capture_load SECONDS     (waits up to 30 s for the monitors to appear)
import AppKit

final class Bar: NSView {
    var frameNo = 0
    override var isOpaque: Bool { true }
    override func draw(_ dirty: NSRect) {
        NSColor(white: 0.12, alpha: 1).setFill()
        bounds.fill()
        let w = bounds.width / 12
        let x = CGFloat(frameNo % 120) / 120 * (bounds.width + w) - w
        NSColor(calibratedRed: 0.3, green: 0.6, blue: 0.9, alpha: 1).setFill()
        NSRect(x: x, y: 0, width: w, height: bounds.height).fill()
        let text = "\(frameNo)" as NSString
        text.draw(at: NSPoint(x: 40, y: 40), withAttributes: [.font: NSFont.monospacedDigitSystemFont(ofSize: 96, weight: .bold),
                                                              .foregroundColor: NSColor.white])
    }
}

let seconds = Double(CommandLine.arguments.dropFirst().first ?? "120") ?? 120
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
var windows: [NSWindow] = []
var views: [Bar] = []
let start = Date()

func attach() {
    let screens = NSScreen.screens.filter { $0.localizedName.hasPrefix("Nimbo ") && !$0.localizedName.contains("·") }
    guard !screens.isEmpty else {
        if Date().timeIntervalSince(start) > 30 { print("capture_load: no Nimbo monitors"); exit(1) }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: attach)
        return
    }
    for s in screens {
        let w = NSWindow(contentRect: s.frame, styleMask: .borderless, backing: .buffered, defer: false)
        w.setFrame(s.frame, display: false)
        w.level = .normal
        w.collectionBehavior = [.canJoinAllSpaces, .stationary]
        let v = Bar(frame: NSRect(origin: .zero, size: s.frame.size))
        w.contentView = v
        w.orderFrontRegardless()
        windows.append(w)
        views.append(v)
    }
    print("capture_load: \(screens.count) monitor(s), \(Int(seconds)) s")
    let timer = Timer(timeInterval: 1.0 / 60, repeats: true) { _ in
        for v in views {
            v.frameNo += 1
            v.needsDisplay = true
        }
    }
    RunLoop.main.add(timer, forMode: .common)
    DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { exit(0) }
}

DispatchQueue.main.async(execute: attach)
app.run()
