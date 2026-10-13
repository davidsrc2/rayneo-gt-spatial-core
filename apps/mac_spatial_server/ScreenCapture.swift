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

    func start(displayID: CGDirectDisplayID, fps: Int) async throws -> (Int, Int) {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        guard let display = content.displays.first(where: { $0.displayID == displayID }) ?? content.displays.first
        else { throw NSError(domain: "ScreenCapture", code: 1, userInfo: [NSLocalizedDescriptionKey: "no display"]) }
        let me = content.applications.filter { $0.processID == getpid() }
        let filter = SCContentFilter(display: display, excludingApplications: me, exceptingWindows: [])

        let cfg = SCStreamConfiguration()
        let scale = CGFloat(filter.pointPixelScale)
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
