// Generates the app icon (AppIcon.icns) and the menu bar template icon from code, so the
// artwork is reproducible and easy to tweak. Run: swift scripts/make_icons.swift
// Design: three floating screens in an arc above a pair of glasses, calm teal/slate palette.

import AppKit
import Foundation

let out = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "assets/icon")
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

func png(_ size: Int, _ draw: (CGContext, CGFloat) -> Void) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let ctx = NSGraphicsContext.current!.cgContext
    ctx.setShouldAntialias(true)
    ctx.interpolationQuality = .high
    draw(ctx, CGFloat(size))
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
    CGColor(red: r, green: g, blue: b, alpha: a)
}

// A screen tilted towards the viewer, centred at (cx, cy), rotated by `angle` (radians).
func screen(_ ctx: CGContext, cx: CGFloat, cy: CGFloat, w: CGFloat, h: CGFloat, angle: CGFloat,
            fill: CGColor, stroke: CGColor, line: CGFloat) {
    ctx.saveGState()
    ctx.translateBy(x: cx, y: cy)
    ctx.rotate(by: angle)
    let r = CGRect(x: -w / 2, y: -h / 2, width: w, height: h)
    let p = CGPath(roundedRect: r, cornerWidth: h * 0.12, cornerHeight: h * 0.12, transform: nil)
    ctx.addPath(p)
    ctx.setFillColor(fill)
    ctx.fillPath()
    ctx.addPath(p)
    ctx.setStrokeColor(stroke)
    ctx.setLineWidth(line)
    ctx.strokePath()
    ctx.restoreGState()
}

func glasses(_ ctx: CGContext, s: CGFloat, cx: CGFloat, cy: CGFloat, width: CGFloat, line: CGFloat, stroke: CGColor) {
    let lensW = width * 0.40, lensH = width * 0.25, gapW = width * 0.20
    ctx.setStrokeColor(stroke)
    ctx.setLineWidth(line)
    ctx.setLineCap(.round)
    for side in [-1.0, 1.0] as [CGFloat] {
        let x = cx + side * (gapW / 2 + lensW / 2)
        let r = CGRect(x: x - lensW / 2, y: cy - lensH / 2, width: lensW, height: lensH)
        ctx.addPath(CGPath(roundedRect: r, cornerWidth: lensH * 0.42, cornerHeight: lensH * 0.42, transform: nil))
        ctx.strokePath()
    }
    // Bridge: a gentle arch between the lenses.
    ctx.move(to: CGPoint(x: cx - gapW / 2, y: cy + lensH * 0.12))
    ctx.addQuadCurve(to: CGPoint(x: cx + gapW / 2, y: cy + lensH * 0.12), control: CGPoint(x: cx, y: cy + lensH * 0.42))
    ctx.strokePath()
}

// MARK: app icon

func appIcon(_ ctx: CGContext, _ s: CGFloat) {
    // macOS icon grid: 824/1024 body with continuous-ish corners and a soft drop shadow.
    let inset = s * 100 / 1024
    let body = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let radius = body.width * 0.225
    let shape = CGPath(roundedRect: body, cornerWidth: radius, cornerHeight: radius, transform: nil)

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -s * 0.012), blur: s * 0.03, color: color(0, 0, 0, 0.35))
    ctx.addPath(shape)
    ctx.setFillColor(color(0.11, 0.15, 0.18))
    ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(shape)
    ctx.clip()
    let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                          colors: [color(0.19, 0.29, 0.33), color(0.10, 0.13, 0.17)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: body.maxY), end: CGPoint(x: 0, y: body.minY), options: [])
    let glow = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                          colors: [color(0.50, 0.70, 0.67, 0.35), color(0.50, 0.70, 0.67, 0)] as CFArray, locations: [0, 1])!
    ctx.drawRadialGradient(glow, startCenter: CGPoint(x: s / 2, y: s * 0.66), startRadius: 0,
                           endCenter: CGPoint(x: s / 2, y: s * 0.66), endRadius: s * 0.42, options: [])

    // Three screens in an arc (left/right tilted, centre in front).
    let teal = color(0.62, 0.80, 0.77, 0.92), tealSoft = color(0.55, 0.72, 0.70, 0.62), edge = color(0.90, 0.97, 0.95, 0.55)
    let cy = s * 0.62
    screen(ctx, cx: s * 0.235, cy: cy - s * 0.035, w: s * 0.20, h: s * 0.14, angle: 0.22, fill: tealSoft, stroke: edge, line: s * 0.006)
    screen(ctx, cx: s * 0.765, cy: cy - s * 0.035, w: s * 0.20, h: s * 0.14, angle: -0.22, fill: tealSoft, stroke: edge, line: s * 0.006)
    screen(ctx, cx: s * 0.50, cy: cy + s * 0.02, w: s * 0.28, h: s * 0.18, angle: 0, fill: teal, stroke: edge, line: s * 0.007)

    glasses(ctx, s: s, cx: s / 2, cy: s * 0.33, width: s * 0.58, line: s * 0.038, stroke: color(0.93, 0.96, 0.95, 0.95))
    ctx.restoreGState()

    // Hairline highlight on the rim.
    ctx.addPath(shape)
    ctx.setStrokeColor(color(1, 1, 1, 0.08))
    ctx.setLineWidth(s * 0.004)
    ctx.strokePath()
}

// MARK: menu bar template (black on transparent; macOS tints it)

func menuBarIcon(_ ctx: CGContext, _ s: CGFloat) {
    let ink = color(0, 0, 0, 1)
    glasses(ctx, s: s, cx: s / 2, cy: s * 0.36, width: s * 0.92, line: s * 0.085, stroke: ink)
    // One screen floating above.
    screen(ctx, cx: s / 2, cy: s * 0.76, w: s * 0.46, h: s * 0.26, angle: 0, fill: color(0, 0, 0, 0), stroke: ink, line: s * 0.075)
}

// MARK: write

let iconset = out.appendingPathComponent("AppIcon.iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let px = base * scale
        let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
        try png(px, appIcon).write(to: iconset.appendingPathComponent(name))
    }
}
try png(1024, appIcon).write(to: out.appendingPathComponent("AppIcon-1024.png"))
try png(18, menuBarIcon).write(to: out.appendingPathComponent("MenuBarIcon.png"))
try png(36, menuBarIcon).write(to: out.appendingPathComponent("MenuBarIcon@2x.png"))
print("wrote \(out.path)")
