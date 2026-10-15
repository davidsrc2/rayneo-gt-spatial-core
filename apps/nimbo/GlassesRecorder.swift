import CoreMedia
import ScreenCaptureKit

// Records what the glasses show (their display, Nimbo's render included) to a .mov in
// Downloads, to send to someone.
@available(macOS 15.0, *)
final class GlassesRecorder: NSObject, SCStreamDelegate, SCRecordingOutputDelegate {
    private var stream: SCStream?
    private(set) var url: URL?

    func start(displayID: CGDirectDisplayID) async throws -> URL {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
            throw NSError(domain: "GlassesRecorder", code: 1, userInfo: [NSLocalizedDescriptionKey: "glasses display not capturable"])
        }
        let cfg = SCStreamConfiguration()
        if let mode = CGDisplayCopyDisplayMode(displayID) {
            cfg.width = mode.pixelWidth
            cfg.height = mode.pixelHeight
        }
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: 60)
        cfg.showsCursor = false
        cfg.queueDepth = 6

        let stamp = DateFormatter()
        stamp.dateFormat = "yyyy-MM-dd 'a las' HH.mm.ss"
        let downloads = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask)[0]
        let url = downloads.appendingPathComponent("Nimbo \(stamp.string(from: Date())).mov")
        let rc = SCRecordingOutputConfiguration()
        rc.outputURL = url
        rc.outputFileType = .mov
        rc.videoCodecType = .h264

        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: cfg, delegate: self)
        try s.addRecordingOutput(SCRecordingOutput(configuration: rc, delegate: self))
        try await s.startCapture()
        stream = s
        self.url = url
        return url
    }

    func stop() async {
        try? await stream?.stopCapture()
        stream = nil
    }
}
