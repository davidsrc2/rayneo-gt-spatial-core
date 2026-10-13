import AppKit
import Carbon.HIToolbox
import Metal
import QuartzCore

final class MetalView: NSView {
    let metalLayer = CAMetalLayer()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer = metalLayer
    }

    required init?(coder: NSCoder) { fatalError("unused") }
}

// Rolling latency statistics, printed once per second.
struct LatencyStats {
    var frames = 0
    var poseAgeSum = 0.0, poseAgeMax = 0.0      // sample -> encode (ms)
    var photonSum = 0.0, photonMax = 0.0        // sample -> target presentation (ms)
    var predSum = 0.0                           // horizon actually extrapolated (ms)
    var presentedN = 0
    var presentedSum = 0.0, presentedMax = 0.0  // sample -> actual presentedTime (ms)
    var targetErrSum = 0.0                      // presentedTime - targetPresentationTimestamp (ms)

    mutating func addPresented(latency: Double, targetError: Double) {
        presentedN += 1
        presentedSum += latency
        presentedMax = max(presentedMax, latency)
        targetErrSum += targetError
    }
    var tracking = false

    mutating func add(poseAge: Double, photon: Double, prediction: Double) {
        frames += 1
        poseAgeSum += poseAge
        poseAgeMax = max(poseAgeMax, poseAge)
        photonSum += photon
        photonMax = max(photonMax, photon)
        predSum += prediction
    }
}

@available(macOS 14.0, *)
final class SpatialServer: NSObject, CAMetalDisplayLinkDelegate {
    private let options: ServerOptions
    private let engine = RNSpatialEngine()
    private var renderer: RNSpatialRenderer!
    private var window: NSWindow!
    private var view: MetalView!
    private var displayLink: CAMetalDisplayLink?
    private var renderThread: Thread?
    private var capture: ScreenCapture?
    private let hotKeys = HotKeys()

    private let statsLock = NSLock()
    private var stats = LatencyStats()
    private var lastCaptureFrames: UInt64 = 0
    private var statsTimer: Timer?

    init(options: ServerOptions) {
        self.options = options
    }

    func start() throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw fail("no Metal device") }

        // Target screen: the glasses, unless debugging in a window.
        let glasses = Displays.glassesScreen()
        let screen: NSScreen
        if options.windowed {
            screen = NSScreen.main!
        } else if let glasses {
            screen = glasses
        } else {
            throw fail("RayNeo display (SmartGlasses) not found; use --windowed to debug")
        }
        let screenID = Displays.id(of: screen)
        if options.unmirror && !options.windowed {
            log(Displays.unmirror(glasses: screenID) ? "mirroring: off (app-only)" : "mirroring: could not disable")
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 1.5))  // let the reconfiguration settle
        }
        log("output: \(screen.localizedName) id=\(screenID) \(Int(screen.frame.width))x\(Int(screen.frame.height))"
            + (Displays.isMirrored(screenID) ? " [mirrored]" : ""))
        if options.refresh120 && !options.windowed {
            log(Displays.setRefresh(screenID, hz: 120) ? "refresh: 120 Hz" : "refresh: 120 Hz unavailable, keeping current")
        }

        // Window: borderless, above everything, click-through and invisible to capture.
        let rect = options.windowed ? NSRect(x: 100, y: 100, width: 1280, height: 720) : screen.frame
        window = NSWindow(contentRect: rect, styleMask: options.windowed ? [.titled] : [.borderless],
                          backing: .buffered, defer: false, screen: screen)
        window.title = "RayNeo spatial server"
        window.backgroundColor = .black
        window.isOpaque = true
        window.sharingType = .none
        if !options.windowed {
            window.level = .screenSaver
            window.ignoresMouseEvents = true
            window.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
            window.setFrame(screen.frame, display: true)
        }
        view = MetalView(frame: NSRect(origin: .zero, size: rect.size))
        window.contentView = view
        window.orderFrontRegardless()

        let layer = view.metalLayer
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        layer.contentsScale = screen.backingScaleFactor
        layer.drawableSize = CGSize(width: rect.width * screen.backingScaleFactor,
                                    height: rect.height * screen.backingScaleFactor)
        layer.maximumDrawableCount = 2  // 3 measured identical; 2 holds less queued work
        layer.displaySyncEnabled = true  // without vsync the scanout time does not move, only the estimate

        renderer = try RNSpatialRenderer(device: device, pixelFormat: .bgra8Unorm)
        let ro = renderer.options
        ro.sideBySide = options.sideBySide
        ro.ipdMeters = options.ipdMM / 1000
        ro.distanceMeters = options.distanceM
        ro.arcDegrees = options.arcDeg
        ro.fovYDegrees = options.fovYDeg
        ro.k1 = options.k1
        ro.k2 = options.k2
        log("render: \(options.sideBySide ? "half-SBS" : "mono") ipd \(options.ipdMM) mm, screen r=\(options.distanceM) m arc \(options.arcDeg)°, fov \(options.fovYDeg)°, k1 \(options.k1) k2 \(options.k2)")

        // IMU engine + calibration.
        do {
            try engine.loadCalibration(fromFile: options.calibrationPath)
            log("calibration: \(options.calibrationPath)")
        } catch {
            if options.calibrationRequired { throw error }
            log("calibration: none (identity)")
        }
        try engine.start()
        log("imu: streaming, keep the glasses still ~6 s for bias calibration")

        // Hotkeys.
        hotKeys.register(keyCode: kVK_ANSI_C, modifiers: cmdKey | optionKey) { [weak self] in
            self?.engine.recenter()
            self?.log("recenter")
        }
        hotKeys.register(keyCode: kVK_ANSI_Q, modifiers: cmdKey | optionKey) {
            NSApp.terminate(nil)
        }

        try startCapture(glasses: glasses)
        startRenderLoop(layer: layer)

        statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in self?.printStats() }
    }

    func stop() {
        displayLink?.invalidate()
        engine.stop()
        let cap = capture
        let done = DispatchSemaphore(value: 0)
        Task.detached {
            await cap?.stop()
            done.signal()
        }
        _ = done.wait(timeout: .now() + 1)
    }

    // MARK: capture

    private func startCapture(glasses: NSScreen?) throws {
        let source = try captureDisplay(glasses: glasses)
        let cap = ScreenCapture { [weak self] pb in self?.renderer.setSourcePixelBuffer(pb) }
        capture = cap
        let sem = DispatchSemaphore(value: 0)
        var result: Result<(Int, Int), Error> = .failure(fail("capture timeout"))
        Task.detached {
            do { result = .success(try await cap.start(displayID: source, fps: 120)) } catch { result = .failure(error) }
            sem.signal()
        }
        // Capture start needs the main run loop only for the permission prompt; poll it.
        while sem.wait(timeout: .now()) == .timedOut {
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
        }
        let (w, h) = try result.get()
        log("capture: display \(source) \(w)x\(h) @120 fps (own windows excluded)")
    }

    private func captureDisplay(glasses: NSScreen?) throws -> CGDirectDisplayID {
        let glassesID = glasses.map(Displays.id(of:))
        switch options.source {
        case "glasses":
            guard let glassesID else { throw fail("no glasses display") }
            return glassesID
        case "builtin":
            guard let s = NSScreen.screens.first(where: { CGDisplayIsBuiltin(Displays.id(of: $0)) != 0 })
            else { throw fail("no built-in display active (mirroring?)") }
            return Displays.id(of: s)
        case "auto":
            // Extended desktop: capture the first non-glasses display. Mirrored: the glasses
            // display itself (our window is excluded, so the desktop behind it is captured).
            if let other = NSScreen.screens.first(where: { Displays.id(of: $0) != glassesID }) {
                return Displays.id(of: other)
            }
            return glassesID ?? CGMainDisplayID()
        default:
            guard let id = UInt32(options.source) else { throw fail("bad --source \(options.source)") }
            return id
        }
    }

    // MARK: render loop

    private func startRenderLoop(layer: CAMetalLayer) {
        let link = CAMetalDisplayLink(metalLayer: layer)
        link.delegate = self
        link.preferredFrameLatency = 1
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 120)
        displayLink = link
        let thread = Thread {
            link.add(to: .current, forMode: .default)
            while !Thread.current.isCancelled {
                RunLoop.current.run(until: Date(timeIntervalSinceNow: 1))
            }
        }
        thread.name = "rayneo.render"
        thread.qualityOfService = .userInteractive
        thread.start()
        renderThread = thread
    }

    func metalDisplayLink(_ link: CAMetalDisplayLink, needsUpdate update: CAMetalDisplayLink.Update) {
        // Map CACurrentMediaTime (display link base) onto the engine's steady clock.
        let nowMedia = CACurrentMediaTime()
        let nowHost = RNSpatialEngine.hostNowNs()
        let targetHost = Int64(nowHost) + Int64((update.targetPresentationTimestamp - nowMedia) * 1e9)

        guard let pose = engine.latestPose() else {
            _ = renderer.render(to: update.drawable, orientation: simd_quatf(ix: 0, iy: 0, iz: 0, r: 1),
                                angularVelocity: .zero, sampleHostNs: 0, targetHostNs: 0)
            return
        }
        // Ground truth: when the frame actually reached the display (same media clock).
        let sampleMedia = nowMedia - Double(Int64(nowHost) - Int64(pose.hostTimeNs)) / 1e9
        let target = update.targetPresentationTimestamp
        update.drawable.addPresentedHandler { [weak self] d in
            guard let self, d.presentedTime > 0 else { return }
            self.statsLock.lock()
            self.stats.addPresented(latency: (d.presentedTime - sampleMedia) * 1e3,
                                    targetError: (d.presentedTime - target) * 1e3)
            self.statsLock.unlock()
        }
        let prediction = renderer.render(to: update.drawable, orientation: pose.orientation,
                                         angularVelocity: pose.angularVelocity,
                                         sampleHostNs: pose.hostTimeNs, targetHostNs: UInt64(max(0, targetHost)))
        let poseAge = Double(Int64(nowHost) - Int64(pose.hostTimeNs)) / 1e6
        let photon = Double(targetHost - Int64(pose.hostTimeNs)) / 1e6
        statsLock.lock()
        stats.add(poseAge: poseAge, photon: photon, prediction: Double(prediction))
        stats.tracking = pose.isTracking
        statsLock.unlock()
    }

    private func printStats() {
        statsLock.lock()
        let s = stats
        stats = LatencyStats()
        statsLock.unlock()
        let capFrames = capture?.frames ?? 0
        let capFps = capFrames &- lastCaptureFrames
        lastCaptureFrames = capFrames
        guard s.frames > 0 else {
            log("render: no frames")
            return
        }
        let n = Double(s.frames)
        let pn = Double(max(1, s.presentedN))
        log(String(format: "%@ render %3d fps | capture %3llu fps | pose age %.1f/%.1f ms | sample->target %.1f ms | sample->presented %.1f/%.1f ms (n=%d, vs target %+.1f) | predicted %.1f ms",
                   s.tracking ? "TRACK" : "CALIB", s.frames, capFps, s.poseAgeSum / n, s.poseAgeMax,
                   s.photonSum / n, s.presentedSum / pn, s.presentedMax, s.presentedN, s.targetErrSum / pn,
                   s.predSum / n))
    }

    // MARK: util

    private func fail(_ msg: String) -> NSError {
        NSError(domain: "SpatialServer", code: 1, userInfo: [NSLocalizedDescriptionKey: msg])
    }

    func log(_ s: String) {
        print(s)
        fflush(stdout)
    }
}
