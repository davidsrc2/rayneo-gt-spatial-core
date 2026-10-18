import CoreMedia
import ScreenCaptureKit

// Display capture via ScreenCaptureKit, excluding this process's windows (no feedback loop
// even when the glasses mirror the captured display).
final class ScreenCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
    private var config: SCStreamConfiguration?
    private var cropScale: CGFloat = 1
    private var windowScale: CGFloat = 2
    private let queue = DispatchQueue(label: "rayneo.capture", qos: .userInteractive)
    private let onFrame: (CVPixelBuffer) -> Void
    private(set) var frames: UInt64 = 0

    init(onFrame: @escaping (CVPixelBuffer) -> Void) {
        self.onFrame = onFrame
    }

    // excludeOwnApp: needed when capturing the display we render on (feedback loop). Off for
    // virtual monitors so this process's test-pattern windows are captured.
    // excludeWindowIDs: windows hidden from this capture regardless of their owner (the
    // renderer window; app-based exclusion misses processes launched from a terminal).
    // crop: only this part of the display (points, display top-left), e.g. one window on it.
    func start(displayID: CGDirectDisplayID, fps: Int, excludeOwnApp: Bool,
               excludeWindowIDs: [CGWindowID] = [], minScale: CGFloat = 1, crop: CGRect? = nil,
               showsCursor: Bool = true) async throws -> (Int, Int) {
        // Freshly created virtual monitors and just-ordered-front windows take a moment to
        // appear in the shareable content; a window missing here would not be excluded.
        var found: (SCShareableContent, SCDisplay)?
        for attempt in 0..<30 {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            if let d = content.displays.first(where: { $0.displayID == displayID }) {
                let ids = Set(content.windows.map(\.windowID))
                let windowsReady = excludeWindowIDs.allSatisfy(ids.contains) || attempt >= 20
                found = (content, d)
                if windowsReady { break }
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        guard let (content, display) = found else {
            throw NSError(domain: "ScreenCapture", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "display \(displayID) not capturable"])
        }
        let pid = getpid()
        let hidden = content.windows.filter {
            excludeWindowIDs.contains($0.windowID) || (excludeOwnApp && $0.owningApplication?.processID == pid)
        }
        let filter = SCContentFilter(display: display, excludingWindows: hidden)

        let cfg = SCStreamConfiguration()
        // Virtual HiDPI monitors may report a point/pixel scale of 1; capture at least at
        // minScale so a 2x backing store is not thrown away.
        let scale = max(CGFloat(filter.pointPixelScale), minScale)
        cfg.width = Int(filter.contentRect.width * scale)
        cfg.height = Int(filter.contentRect.height * scale)
        // The display's real backing size when it is larger (a HiDPI mode settled after the
        // shareable content was read), and scale to fit instead of cropping: a frame smaller
        // than the display would otherwise show only its top-left part.
        if let mode = CGDisplayCopyDisplayMode(displayID), mode.pixelWidth > cfg.width {
            cfg.width = mode.pixelWidth
            cfg.height = mode.pixelHeight
        }
        if let crop {
            cropScale = scale
            cfg.sourceRect = crop
            cfg.width = Int(crop.width * scale)
            cfg.height = Int(crop.height * scale)
        }
        cfg.scalesToFit = true
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.pixelFormat = kCVPixelFormatType_32BGRA
        // The Mac's screen is Display P3: its pixels drawn as sRGB (the renderer's and the
        // glasses' space) came out oversaturated and too contrasty. Converted here instead.
        cfg.colorSpaceName = CGColorSpace.sRGB
        cfg.queueDepth = 4
        cfg.showsCursor = showsCursor  // false: the renderer draws it at the glasses' rate

        config = cfg
        let s = SCStream(filter: filter, configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture()
        stream = s
        return (cfg.width, cfg.height)
    }

    // A new crop for a display capture started with one (the window moved or was resized).
    func updateCrop(_ crop: CGRect) async {
        guard let stream, let cfg = config else { return }
        cfg.sourceRect = crop
        cfg.width = Int(crop.width * cropScale)
        cfg.height = Int(crop.height * cropScale)
        try? await stream.updateConfiguration(cfg)
    }

    // A window capture's window was resized: frames at its new size.
    func updateWindowSize(_ size: CGSize) async {
        guard let stream, let cfg = config else { return }
        cfg.width = Int(size.width * windowScale)
        cfg.height = Int(size.height * windowScale)
        try? await stream.updateConfiguration(cfg)
    }

    // A window capture moved to another window (its filter swapped, the stream kept).
    func retarget(windowID: CGWindowID) async {
        guard let stream, let cfg = config,
              let content = try? await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: false),
              let window = content.windows.first(where: { $0.windowID == windowID }) else { return }
        cfg.width = Int(window.frame.width * windowScale)
        cfg.height = Int(window.frame.height * windowScale)
        try? await stream.updateContentFilter(SCContentFilter(desktopIndependentWindow: window))
        try? await stream.updateConfiguration(cfg)
    }

    // A single window, wherever it is (covered by others too): a floating panel in space.
    func start(windowID: CGWindowID, fps: Int) async throws -> (Int, Int) {
        let content = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: false)
        guard let window = content.windows.first(where: { $0.windowID == windowID }) else {
            throw NSError(domain: "ScreenCapture", code: 2, userInfo: [NSLocalizedDescriptionKey: "window \(windowID) not capturable"])
        }
        let filter = SCContentFilter(desktopIndependentWindow: window)
        let cfg = SCStreamConfiguration()
        let scale = max(CGFloat(filter.pointPixelScale), 2)
        windowScale = scale
        cfg.width = Int(window.frame.width * scale)
        cfg.height = Int(window.frame.height * scale)
        cfg.scalesToFit = true
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.pixelFormat = kCVPixelFormatType_32BGRA
        // The Mac's screen is Display P3: its pixels drawn as sRGB (the renderer's and the
        // glasses' space) came out oversaturated and too contrasty. Converted here instead.
        cfg.colorSpaceName = CGColorSpace.sRGB
        cfg.queueDepth = 4
        cfg.showsCursor = true
        config = cfg
        let s = SCStream(filter: filter, configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture()
        stream = s
        return (cfg.width, cfg.height)
    }

    struct WindowInfo: Hashable {
        let id: CGWindowID
        let title: String
        let app: String
    }

    // Windows worth floating: titled app windows, not tiny, not ours. Windows on other Spaces or
    // behind a full-screen app count too (capture works there); untitled ones only when on screen.
    static func windows() async -> [WindowInfo] {
        guard let content = try? await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: false)
        else { return [] }
        let me = getpid()
        return content.windows
            .filter { $0.frame.width >= 200 && $0.frame.height >= 120 && $0.windowLayer == 0 }
            .filter { $0.owningApplication?.processID != me }
            .filter { !($0.title ?? "").isEmpty || $0.isOnScreen }
            .map { WindowInfo(id: $0.windowID, title: $0.title ?? "", app: $0.owningApplication?.applicationName ?? "") }
            .filter { !$0.app.isEmpty }
    }

    // The first window whose title or app contains `text` (case-insensitive).
    static func window(matching text: String) async -> WindowInfo? {
        let t = text.lowercased()
        return await windows().first { $0.title.lowercased().contains(t) || $0.app.lowercased().contains(t) }
    }

    // A remembered window: the same app and title, else the same app's first window (a browser
    // tab title changes with every video).
    static func window(app: String, title: String) async -> WindowInfo? {
        let all = await windows().filter { $0.app == app }
        return all.first { $0.title == title } ?? all.first
    }

    func stop() async {
        try? await stream?.stopCapture()
        stream = nil
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sb.isValid, let pb = sb.imageBuffer else { return }
        // Only complete frames carry new pixels; idle/blank frames reuse the last texture.
        if let info = (CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]])?.first,
           let raw = info[.status] as? Int, SCFrameStatus(rawValue: raw) != .complete {
            return
        }
        frames &+= 1
        onFrame(pb)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        FileHandle.standardError.write("capture stopped: \(error.localizedDescription)\n".data(using: .utf8)!)
    }
}
