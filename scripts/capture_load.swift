// Measurement load: on every Nimbo virtual monitor ("Nimbo N"), a full-screen window that
// changes every frame at 60 fps (a sweeping bar and a frame counter), so ScreenCaptureKit
// delivers 60 frames per source per second, the same in every session. With the monitors
// static, captures drop to a few frames per second and missed refreshes fall with them: runs
// could not be compared.
//
//   swiftc -O scripts/capture_load.swift -o build/capture_load
//   build/capture_load SECONDS [--static] [--prefix NAME]
//     waits up to 30 s for the monitors to appear; --static: a fixed, non-periodic texture
//     instead (optical measurements, scripts/optical_motion.swift: texture to follow, nothing
//     changing); --prefix: the monitors' name prefix (default "Nimbo ", another app's otherwise);
//     --all: every screen but the glasses' own (whatever another app shows on the glasses)
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
let staticTexture = CommandLine.arguments.contains("--static")
let prefix = CommandLine.arguments.firstIndex(of: "--prefix").flatMap { i in
    i + 1 < CommandLine.arguments.count ? CommandLine.arguments[i + 1] : nil } ?? "Nimbo "

// Value noise over several octaves (seeded, the same every run): blobs and edges at every
// scale, no period for a correlation to lock onto.
func noiseImage(width: Int, height: Int) -> NSImage {
    var rng: UInt64 = 0x9E3779B97F4A7C15
    func rand() -> Double { rng = rng &* 6364136223846793005 &+ 1442695040888963407; return Double(rng >> 11) / Double(1 << 53) }
    var pixels = [UInt8](repeating: 0, count: width * height * 4)
    var acc = [Double](repeating: 0, count: width * height)
    var amp = 1.0, cell = 256
    while cell >= 4 {
        let gw = width / cell + 2, gh = height / cell + 2
        let grid = (0..<(gw * gh)).map { _ in rand() }
        for y in 0..<height { for x in 0..<width {
            let fx = Double(x) / Double(cell), fy = Double(y) / Double(cell)
            let ix = Int(fx), iy = Int(fy), tx = fx - Double(ix), ty = fy - Double(iy)
            let a = grid[iy * gw + ix], b = grid[iy * gw + ix + 1], c = grid[(iy + 1) * gw + ix], d = grid[(iy + 1) * gw + ix + 1]
            let sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty)
            acc[y * width + x] += amp * (a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy)
        } }
        amp *= 0.6
        cell /= 2
    }
    let lo = acc.min() ?? 0, hi = acc.max() ?? 1
    for i in 0..<(width * height) {
        let v = UInt8(max(0, min(255, (acc[i] - lo) / (hi - lo) * 255)))
        pixels[4 * i] = v; pixels[4 * i + 1] = v; pixels[4 * i + 2] = v; pixels[4 * i + 3] = 255
    }
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: width * 4, bitsPerPixel: 32)!
    pixels.withUnsafeBytes { memcpy(rep.bitmapData!, $0.baseAddress!, pixels.count) }
    let img = NSImage(size: NSSize(width: width, height: height))
    img.addRepresentation(rep)
    return img
}

final class Still: NSView {
    let image: NSImage
    init(frame: NSRect, image: NSImage) { self.image = image; super.init(frame: frame) }
    required init?(coder: NSCoder) { fatalError("unused") }
    override var isOpaque: Bool { true }
    override func draw(_ dirty: NSRect) { image.draw(in: bounds) }
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
var windows: [NSWindow] = []
var views: [Bar] = []
let start = Date()

func attach() {
    let all = CommandLine.arguments.contains("--all")
    let screens = NSScreen.screens.filter {
        all ? !$0.localizedName.localizedCaseInsensitiveContains("glasses")
            : $0.localizedName.hasPrefix(prefix) && !$0.localizedName.contains("·")
    }
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
        w.orderFrontRegardless()
        windows.append(w)
        if staticTexture {
            w.contentView = Still(frame: NSRect(origin: .zero, size: s.frame.size), image: noiseImage(width: 1024, height: 576))
            continue
        }
        let v = Bar(frame: NSRect(origin: .zero, size: s.frame.size))
        w.contentView = v
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
