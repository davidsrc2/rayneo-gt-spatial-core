import CoreMedia
import ScreenCaptureKit

// Display capture via ScreenCaptureKit, excluding this process's windows (no feedback loop
// even when the glasses mirror the captured display).
final class ScreenCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
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
    func start(displayID: CGDirectDisplayID, fps: Int, excludeOwnApp: Bool,
               excludeWindowIDs: [CGWindowID] = [], minScale: CGFloat = 1) async throws -> (Int, Int) {
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
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.pixelFormat = kCVPixelFormatType_32BGRA
        cfg.queueDepth = 4
        cfg.showsCursor = true

        let s = SCStream(filter: filter, configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture()
        stream = s
        return (cfg.width, cfg.height)
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
