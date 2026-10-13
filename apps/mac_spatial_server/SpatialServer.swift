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
    var tracking = false

    mutating func add(poseAge: Double, photon: Double, prediction: Double) {
        frames += 1
        poseAgeSum += poseAge
        poseAgeMax = max(poseAgeMax, poseAge)
        photonSum += photon
        photonMax = max(photonMax, photon)
        predSum += prediction
    }

    mutating func addPresented(latency: Double, targetError: Double) {
        presentedN += 1
        presentedSum += latency
        presentedMax = max(presentedMax, latency)
        targetErrSum += targetError
    }
}

@available(macOS 14.0, *)
final class SpatialServer: NSObject, CAMetalDisplayLinkDelegate {
    private let options: ServerOptions
    private var pose: PoseSource!
    private var renderer: RNSpatialRenderer!
    private var window: NSWindow!
    private var view: MetalView!
    private var displayLink: CAMetalDisplayLink?
    private var renderThread: Thread?
    private var captures: [ScreenCapture] = []
    private var desktop: VirtualDesktop?
    private var patternWindows: [NSWindow] = []
    private var panelDisplays: [CGDirectDisplayID] = []  // left -> right
    private var gazeTimer: Timer?
    private var lastGazePanel = -2
    private var physicalDisplay: CGDirectDisplayID?  // glasses surface when it is not a panel
    private let hotKeys = HotKeys()
    private var statusMenu: StatusMenu?

    private let statsLock = NSLock()
    private var stats = LatencyStats()
    private var lastCaptureFrames: UInt64 = 0
    private var statsTimer: Timer?
    private var calibSeconds = 0

    init(options: ServerOptions) {
        self.options = options
    }

    func start() throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw fail("no Metal device") }

        // Output: the glasses, or a debug window on the Mac screen.
        let glasses = Displays.glassesScreen()
        let screen: NSScreen
        if options.windowed {
            screen = NSScreen.main!
        } else if let glasses {
            screen = glasses
        } else {
            throw fail("RayNeo display (SmartGlasses) not found. Without the glasses: --windowed --sim")
        }
        let screenID = Displays.id(of: screen)
        if options.unmirror && !options.windowed {
            log(Displays.unmirror(glasses: screenID) ? "mirroring: off (app-only)" : "mirroring: could not disable")
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 1.5))  // let the reconfiguration settle
        }
        log("output: \(screen.localizedName) id=\(screenID) \(Int(screen.frame.width))x\(Int(screen.frame.height))"
            + (Displays.isMirrored(screenID) ? " [mirrored]" : "") + (options.windowed ? " [window]" : ""))
        if options.refresh120 && !options.windowed {
            log(Displays.setRefresh(screenID, hz: 120) ? "refresh: 120 Hz" : "refresh: 120 Hz unavailable, keeping current")
        }

        // Pose first: the IMU needs ~6 s of rest, let it start while the rest comes up.
        if options.sim {
            pose = SimPoseSource(sweepAmplitudeDeg: options.simSweepDeg)
            log("tracking: simulated head" + (options.simSweepDeg > 0 ? " (sweep ±\(options.simSweepDeg)°)" : " (Cmd+Opt+arrows)"))
        } else {
            let imu = ImuPoseSource(calibrationPath: options.calibrationPath,
                                    calibrationRequired: options.calibrationRequired)
            pose = imu
        }
        try pose.start()
        if let imu = pose as? ImuPoseSource {
            log("calibration: " + (imu.calibrationLoaded ? options.calibrationPath : "none (identity)"))
            log("imu: streaming, leave the glasses still on the table ~6 s until TRACK")
        }

        // Virtual monitors before the window, so the layout is settled when we draw.
        if options.displays > 0 {
            let vd = try VirtualDesktop(count: options.displays, width: options.vdWidth, height: options.vdHeight,
                                        hiDPI: options.hiDPI, refresh: 60)
            desktop = vd
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.5))
            let anchor = CGMainDisplayID()
            let virtualMain = !options.includeMain && !options.windowed
            if virtualMain {
                vd.arrangeAsMain(tucking: screenID)
            } else {
                vd.arrange(rightOf: anchor)
            }
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 1.0))  // NSScreen list catches up
            log("virtual monitors: " + vd.ids.map { id in
                let b = CGDisplayBounds(id)
                return "\(id)@\(Int(b.minX)),\(Int(b.minY)) \(Int(b.width))x\(Int(b.height))"
            }.joined(separator: "  ") + (virtualMain ? "  (panel 1 is the main display; glasses surface tucked away)"
                                                       : "  (right of display \(anchor))"))
            if virtualMain {
                physicalDisplay = screenID
                let target = CGDisplayBounds(vd.ids[0])
                CGWarpMouseCursorPosition(CGPoint(x: target.midX, y: target.midY))
                if options.gatherWindows {
                    if WindowGatherer.isTrusted(prompt: true) {
                        let n = WindowGatherer.gather(from: CGDisplayBounds(screenID), into: target)
                        log("windows: moved \(n) window(s) to panel 1")
                    } else {
                        log("windows: allow Terminal in System Settings > Privacy & Security > Accessibility " +
                            "to bring your open windows into the panels (new windows open there anyway)")
                    }
                }
            }
            // The main display (your real desktop) sits left of the virtual row, matching
            // where the cursor goes when it leaves it to the right.
            panelDisplays = (options.includeMain ? [anchor] : []) + vd.ids
            let offset = options.includeMain ? 1 : 0
            if options.testPattern {
                for (i, id) in vd.ids.enumerated() {
                    guard let s = vd.screen(for: id) else { continue }
                    patternWindows.append(TestPatternView.makeWindow(on: s, index: i + offset, total: panelDisplays.count))
                }
            }
        }

        // Render window.
        let rect = options.windowed ? NSRect(x: 80, y: 80, width: 1280, height: 720) : screen.frame
        window = NSWindow(contentRect: rect, styleMask: options.windowed ? [.titled, .miniaturizable] : [.borderless],
                          backing: .buffered, defer: false, screen: screen)
        window.title = "RayNeo spatial server (preview)"
        window.backgroundColor = .black
        window.isOpaque = true
        window.isReleasedWhenClosed = false
        if !options.windowed {
            // Above everything, click-through, invisible to capture (no feedback loop).
            window.sharingType = .none
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
        let sbs = options.sideBySide ?? !options.windowed
        let ro = renderer.options
        ro.sideBySide = sbs
        ro.ipdMeters = options.ipdMM / 1000
        ro.distanceMeters = options.distanceM
        ro.arcDegrees = options.panelArcDeg
        ro.gapDegrees = options.gapDeg
        ro.curved = options.curved
        ro.fovYDegrees = options.fovYDeg
        ro.k1 = options.k1
        ro.k2 = options.k2
        let panels = max(1, panelDisplays.count)
        renderer.panelCount = UInt(panels)
        if options.fit && options.arcDeg == nil {
            ro.arcDegrees = renderer.fitArcDegrees(forPanels: UInt(panels))
        }
        log("render: \(sbs ? "half-SBS" : "mono"), \(panels) \(options.curved ? "curved" : "flat") panel(s) of \(String(format: "%.1f", ro.arcDegrees))° gap \(options.gapDeg)° at \(options.distanceM) m, ipd \(options.ipdMM) mm, fov \(options.fovYDeg)°, k1 \(options.k1) k2 \(options.k2)")

        registerHotKeys()
        statusMenu = StatusMenu { [weak self] in self?.recenter() }
        if !options.windowed {
            log(HardwareCursor.hide() ? "cursor: hardware cursor hidden (the panels show the real one)"
                                      : "cursor: could not hide the hardware cursor")
        }
        try startCaptures(glasses: glasses)
        startRenderLoop(layer: layer)

        statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in self?.printStats() }
        if (options.gazeCursor && panelDisplays.count > 1) || physicalDisplay != nil {
            gazeTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
                self?.followGaze()
            }
            log("cursor: follows the panel you look at (--no-gaze-cursor to disable)")
        }
    }

    // MARK: gaze cursor

    // When the gaze moves to another panel, bring the cursor along, keeping its relative
    // position (the main display is invisible behind the glasses image otherwise).
    private func followGaze() {
        // The glasses surface is head-locked: a cursor there would be a second, floating one.
        if let physical = physicalDisplay, let cursor = CGEvent(source: nil)?.location,
           CGDisplayBounds(physical).contains(cursor) {
            let gaze = pose.latest().map { renderer.panel(atGaze: $0.orientation) } ?? 0
            let b = CGDisplayBounds(panelDisplays[max(0, min(panelDisplays.count - 1, gaze))])
            CGWarpMouseCursorPosition(CGPoint(x: b.midX, y: b.midY))
            CGAssociateMouseAndMouseCursorPosition(1)
            return
        }
        guard options.gazeCursor, let p = pose.latest(), p.tracking else { return }
        let panel = renderer.panel(atGaze: p.orientation)
        guard panel != lastGazePanel else { return }
        lastGazePanel = panel
        guard panel >= 0, panel < panelDisplays.count, let cursor = CGEvent(source: nil)?.location else { return }
        let target = CGDisplayBounds(panelDisplays[panel])
        if target.contains(cursor) { return }
        var rel = CGPoint(x: 0.5, y: 0.5)
        if let from = panelDisplays.map(CGDisplayBounds).first(where: { $0.contains(cursor) }) {
            rel = CGPoint(x: (cursor.x - from.minX) / from.width, y: (cursor.y - from.minY) / from.height)
        }
        CGWarpMouseCursorPosition(CGPoint(x: target.minX + rel.x * target.width, y: target.minY + rel.y * target.height))
        CGAssociateMouseAndMouseCursorPosition(1)
    }

    func stop() {
        HardwareCursor.show()
        gazeTimer?.invalidate()
        displayLink?.invalidate()
        pose?.stop()
        physicalDisplay = nil
        let caps = captures
        let done = DispatchSemaphore(value: 0)
        Task.detached {
            for c in caps { await c.stop() }
            done.signal()
        }
        _ = done.wait(timeout: .now() + 2)
        patternWindows.forEach { $0.close() }
        desktop = nil  // virtual monitors disappear here
    }

    // MARK: input

    func recenter() {
        pose.recenter()
        lastGazePanel = -2  // re-evaluate: the cursor follows to the panel in front
        log("recenter")
    }

    private func registerHotKeys() {
        let mods = cmdKey | optionKey
        hotKeys.register(keyCode: kVK_ANSI_C, modifiers: mods) { [weak self] in self?.recenter() }
        hotKeys.register(keyCode: kVK_ANSI_Q, modifiers: mods) { NSApp.terminate(nil) }
        if let sim = pose as? SimPoseSource {
            let step: Float = 10
            hotKeys.register(keyCode: kVK_LeftArrow, modifiers: mods) { sim.nudge(yaw: step, pitch: 0) }
            hotKeys.register(keyCode: kVK_RightArrow, modifiers: mods) { sim.nudge(yaw: -step, pitch: 0) }
            hotKeys.register(keyCode: kVK_UpArrow, modifiers: mods) { sim.nudge(yaw: 0, pitch: step) }
            hotKeys.register(keyCode: kVK_DownArrow, modifiers: mods) { sim.nudge(yaw: 0, pitch: -step) }
        }
    }

    // MARK: capture

    private func startCaptures(glasses: NSScreen?) throws {
        // (display, panel, excludeOwnApp)
        var plan: [(CGDirectDisplayID, Int, Bool)] = []
        if desktop != nil {
            // Own windows: excluded on the main display (the renderer may sit there), kept on
            // virtual monitors (test patterns).
            let mainID = options.includeMain ? panelDisplays.first : nil
            plan = panelDisplays.enumerated().map { ($0.element, $0.offset, $0.element == mainID) }
        } else {
            plan = [(try captureDisplay(glasses: glasses), 0, true)]
        }
        let fps = desktop != nil ? 60 : 120
        let virtualIDs = Set(desktop?.ids ?? [])
        let hiDPI = options.hiDPI
        for (id, panel, exclude) in plan {
            let isVirtual = virtualIDs.contains(id)
            let cap = ScreenCapture { [weak self] pb in self?.renderer.setSourcePixelBuffer(pb, forPanel: UInt(panel)) }
            captures.append(cap)
            let renderWindow = CGWindowID(window.windowNumber)
            let (w, h) = try awaitOnMain {
                try await cap.start(displayID: id, fps: fps, excludeOwnApp: exclude, excludeWindowIDs: [renderWindow],
                                    minScale: isVirtual && hiDPI ? 2 : 1)
            }
            log("capture: panel \(panel + 1) <- display \(id) \(w)x\(h) @\(fps) fps")
        }
    }

    // Runs async work while keeping the main run loop alive (TCC prompt, display updates).
    private func awaitOnMain<T>(_ work: @escaping () async throws -> T) throws -> T {
        let sem = DispatchSemaphore(value: 0)
        var result: Result<T, Error> = .failure(fail("timeout"))
        Task.detached {
            do { result = .success(try await work()) } catch { result = .failure(error) }
            sem.signal()
        }
        while sem.wait(timeout: .now()) == .timedOut {
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
        }
        return try result.get()
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

        guard let p = pose.latest() else {
            _ = renderer.render(to: update.drawable, orientation: simd_quatf(ix: 0, iy: 0, iz: 0, r: 1),
                                angularVelocity: .zero, sampleHostNs: 0, targetHostNs: 0)
            return
        }
        // Ground truth: when the frame actually reached the display (same media clock).
        let sampleMedia = nowMedia - Double(Int64(nowHost) - Int64(p.hostTimeNs)) / 1e9
        let target = update.targetPresentationTimestamp
        update.drawable.addPresentedHandler { [weak self] d in
            guard let self, d.presentedTime > 0 else { return }
            self.statsLock.lock()
            self.stats.addPresented(latency: (d.presentedTime - sampleMedia) * 1e3,
                                    targetError: (d.presentedTime - target) * 1e3)
            self.statsLock.unlock()
        }
        let prediction = renderer.render(to: update.drawable, orientation: p.orientation,
                                         angularVelocity: p.angularVelocity,
                                         sampleHostNs: p.hostTimeNs, targetHostNs: UInt64(max(0, targetHost)))
        let poseAge = Double(Int64(nowHost) - Int64(p.hostTimeNs)) / 1e6
        let photon = Double(targetHost - Int64(p.hostTimeNs)) / 1e6
        statsLock.lock()
        stats.add(poseAge: poseAge, photon: photon, prediction: Double(prediction))
        stats.tracking = p.tracking
        statsLock.unlock()
    }

    private func printStats() {
        statsLock.lock()
        let s = stats
        stats = LatencyStats()
        statsLock.unlock()
        let capFrames = captures.reduce(UInt64(0)) { $0 &+ $1.frames }
        let capFps = capFrames &- lastCaptureFrames
        lastCaptureFrames = capFrames
        guard s.frames > 0 else {
            log("render: no frames")
            return
        }
        if !s.tracking && pose is ImuPoseSource {
            calibSeconds += 1
            if calibSeconds % 3 == 1 {
                log("CALIB: head tracking starts after ~6 s with the glasses still on the table" +
                    " (or ~14 s if you are already wearing them)")
            }
        }
        let n = Double(s.frames)
        let pn = Double(max(1, s.presentedN))
        log(String(format: "%@ render %3d fps | capture %3llu fps (%d src) | pose age %.1f/%.1f ms | sample->presented %.1f/%.1f ms | predicted %.1f ms",
                   s.tracking ? "TRACK" : "CALIB", s.frames, capFps, captures.count, s.poseAgeSum / n, s.poseAgeMax,
                   s.presentedSum / pn, s.presentedMax, s.predSum / n))
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
