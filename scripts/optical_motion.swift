// Optical measurement of what the glasses show, for any app driving them (Nimbo with
// --capturable, RealWarp...): the glasses display captured at its refresh rate, and for every
// frame the image's shift against the previous one by phase correlation (sub-pixel) on a
// 512x512 crop of its centre. Written as CSV: time (s), dx, dy (px, right / down), peak
// (correlation strength; low = no texture or a change of content, not motion), status
// (complete / idle = unchanged). scripts/optical_stats.py turns it into wobble, jitter, drift.
//
//   swiftc -O scripts/optical_motion.swift -o build/optical_motion
//   build/optical_motion SECONDS OUT.csv [DISPLAY_ID]   (default: the 1920x1080 non-built-in display
//                                                    that is not a Nimbo monitor)
import Accelerate
import CoreMedia
import Foundation
import ScreenCaptureKit

let args = CommandLine.arguments
guard args.count >= 3, let seconds = Double(args[1]) else {
    print("usage: optical_motion SECONDS OUT.csv [DISPLAY_ID]")
    exit(2)
}
let outPath = args[2]
let wantedID = args.count > 3 ? UInt32(args[3]) : nil

let n = 512, log2n: vDSP_Length = 9
final class Correlator {
    let setup = vDSP_create_fftsetup(9, FFTRadix(kFFTRadix2))!
    var window = [Float](repeating: 0, count: 512 * 512)
    var prevRe = [Float](), prevIm = [Float]()
    var lastDx = 0.0, lastDy = 0.0  // the head does not jump: the peak is searched near the last shift
    init() {
        for y in 0..<n { for x in 0..<n {
            let wx = 0.5 - 0.5 * cos(2 * Float.pi * Float(x) / Float(n - 1))
            let wy = 0.5 - 0.5 * cos(2 * Float.pi * Float(y) / Float(n - 1))
            window[y * n + x] = wx * wy
        } }
    }
    // Shift of `img` against the previous image (px), and the peak's height.
    func shift(_ img: [Float]) -> (Double, Double, Double)? {
        var re = [Float](repeating: 0, count: n * n), im = [Float](repeating: 0, count: n * n)
        var mean: Float = 0
        vDSP_meanv(img, 1, &mean, vDSP_Length(n * n))
        for i in 0..<(n * n) { re[i] = (img[i] - mean) * window[i] }
        re.withUnsafeMutableBufferPointer { r in im.withUnsafeMutableBufferPointer { i in
            var s = DSPSplitComplex(realp: r.baseAddress!, imagp: i.baseAddress!)
            vDSP_fft2d_zip(setup, &s, 1, 0, log2n, log2n, FFTDirection(kFFTDirection_Forward))
        } }
        defer { prevRe = re; prevIm = im }
        guard !prevRe.isEmpty else { return nil }
        // Normalised cross-power spectrum: prev * conj(cur) / |...|.
        var cr = [Float](repeating: 0, count: n * n), ci = [Float](repeating: 0, count: n * n)
        for k in 0..<(n * n) {
            let a = prevRe[k], b = prevIm[k], c = re[k], d = -im[k]
            let x = a * c - b * d, y = a * d + b * c
            let m = max(1e-12, (x * x + y * y).squareRoot())
            cr[k] = x / m
            ci[k] = y / m
        }
        cr.withUnsafeMutableBufferPointer { r in ci.withUnsafeMutableBufferPointer { i in
            var s = DSPSplitComplex(realp: r.baseAddress!, imagp: i.baseAddress!)
            vDSP_fft2d_zip(setup, &s, 1, 0, log2n, log2n, FFTDirection(kFFTDirection_Inverse))
        } }
        // The peak within 40 px of the last shift (as prev - cur: its negative); a periodic
        // pattern elsewhere in the image correlates too and must not win.
        var best: Float = -.infinity, px = 0, py = 0
        let cx = Int((-lastDx).rounded()), cy = Int((-lastDy).rounded())
        for oy in -40...40 { for ox in -40...40 {
            let x = (cx + ox + n) % n, y = (cy + oy + n) % n
            let v = cr[y * n + x]
            if v > best { best = v; px = x; py = y }
        } }
        func at(_ x: Int, _ y: Int) -> Float { cr[((y + n) % n) * n + ((x + n) % n)] }
        func sub(_ m: Float, _ c: Float, _ p: Float) -> Double {
            let den = m - 2 * c + p
            return abs(den) < 1e-12 ? 0 : Double(0.5 * (m - p) / den)
        }
        var dx = Double(px) + sub(at(px - 1, py), best, at(px + 1, py))
        var dy = Double(py) + sub(at(px, py - 1), best, at(px, py + 1))
        if dx > Double(n / 2) { dx -= Double(n) }
        if dy > Double(n / 2) { dy -= Double(n) }
        // prev * conj(cur) peaks at prev - cur: the image moved by its negative.
        let peak = Double(best) / Double(n * n)
        if !(peak >= 0.15) {  // no texture (black: looking off the panels) or new content: no reading
            lastDx = 0
            lastDy = 0
            return (0, 0, peak.isFinite ? peak : 0)
        }
        lastDx = -dx
        lastDy = -dy
        return (-dx, -dy, peak)
    }
}

final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    let corr = Correlator()
    var lines = ["t,dx,dy,peak,status"]
    var t0: Double?
    let queue = DispatchQueue(label: "optical")
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let info = (CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]])?.first,
              let raw = info[.status] as? Int, let status = SCFrameStatus(rawValue: raw) else { return }
        let t = CMSampleBufferGetPresentationTimeStamp(sb).seconds
        if t0 == nil { t0 = t }
        let rt = t - (t0 ?? t)
        if status == .idle {  // nothing changed on the display this refresh
            lines.append(String(format: "%.6f,0,0,1,idle", rt))
            return
        }
        guard status == .complete, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0)
        let stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
        // Half resolution (2x2 averages), its central 512x512: most of the image, half the
        // motion per frame. Shifts are reported in full-resolution pixels.
        guard w >= 2 * n, h >= n + 32, let base = CVPixelBufferGetBaseAddressOfPlane(pb, 0)?.assumingMemoryBound(to: UInt8.self) else { return }
        let hw = w / 2, hh = h / 2
        let x0 = (hw - n) / 2, y0 = max(0, (hh - n) / 2)
        var img = [Float](repeating: 0, count: n * n)
        for y in 0..<min(n, hh - y0) {
            let r0 = base + 2 * (y0 + y) * stride, r1 = r0 + stride
            for x in 0..<n {
                let sx = 2 * (x0 + x)
                img[y * n + x] = (Float(r0[sx]) + Float(r0[sx + 1]) + Float(r1[sx]) + Float(r1[sx + 1])) * 0.25
            }
        }
        if let (dx, dy, peak) = corr.shift(img) {
            lines.append(String(format: "%.6f,%.4f,%.4f,%.4f,complete", rt, 2 * dx, 2 * dy, peak))
        }
    }
}

let sema = DispatchSemaphore(value: 0)
let cap = Capture()
var stream: SCStream?
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        let names = Dictionary(uniqueKeysWithValues: NSScreenNames())
        guard let display = content.displays.first(where: { d in
            if let wantedID { return d.displayID == wantedID }
            return CGDisplayIsBuiltin(d.displayID) == 0 && d.width == 1920 && d.height == 1080 &&
                !(names[d.displayID] ?? "").hasPrefix("Nimbo")
        }) else {
            print("optical_motion: glasses display not found (\(content.displays.map { $0.displayID }))")
            exit(1)
        }
        let cfg = SCStreamConfiguration()
        let mode = CGDisplayCopyDisplayMode(display.displayID)
        cfg.width = mode?.pixelWidth ?? 1920
        cfg.height = mode?.pixelHeight ?? 1080
        cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(max(60, Int(mode?.refreshRate ?? 120))))
        cfg.queueDepth = 8
        cfg.showsCursor = false
        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: cfg, delegate: cap)
        try s.addStreamOutput(cap, type: .screen, sampleHandlerQueue: cap.queue)
        try await s.startCapture()
        stream = s
        print("optical_motion: display \(display.displayID) \(cfg.width)x\(cfg.height) @\(Int(mode?.refreshRate ?? 0)) Hz, \(Int(seconds)) s")
        try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
        try await s.stopCapture()
        cap.queue.sync {
            try? (cap.lines.joined(separator: "\n") + "\n").write(toFile: outPath, atomically: true, encoding: .utf8)
        }
        print("optical_motion: \(cap.lines.count - 1) frames -> \(outPath)")
        sema.signal()
    } catch {
        print("optical_motion: \(error.localizedDescription)")
        exit(1)
    }
}

// Display names by id (NSScreen), to leave Nimbo's own virtual monitors out.
func NSScreenNames() -> [(CGDirectDisplayID, String)] {
    var ids = [CGDirectDisplayID](repeating: 0, count: 16), count: UInt32 = 0
    CGGetOnlineDisplayList(16, &ids, &count)
    return ids.prefix(Int(count)).map { id in
        var name = ""
        if let info = CoreDisplayInfo(id) { name = info }
        return (id, name)
    }
}

func CoreDisplayInfo(_ id: CGDirectDisplayID) -> String? {
    // Virtual displays report no vendor (0) and Nimbo names them "Nimbo N"; the glasses have a vendor.
    CGDisplayVendorNumber(id) == 0 ? "Nimbo" : nil
}

sema.wait()
