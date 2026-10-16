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
    // Frame pacing on the display (consecutive presented frames, idle gaps excluded): an uneven
    // rhythm reads as vibration while the head moves.
    var intervalN = 0
    var intervalSum = 0.0, intervalSq = 0.0, intervalMax = 0.0  // ms
    var targetErrSum = 0.0                      // presentedTime - targetPresentationTimestamp (ms)
    var tracking = false
    var idle = 0                                // frames skipped: nothing changed
    // With the head still (< 1 deg/s): frame-to-frame change of the pose drawn, i.e. how much
    // the panels wobble in the room (deg rms).
    var stillN = 0
    var stillSq = 0.0

    mutating func add(poseAge: Double, photon: Double, prediction: Double) {
        frames += 1
        poseAgeSum += poseAge
        poseAgeMax = max(poseAgeMax, poseAge)
        photonSum += photon
        photonMax = max(photonMax, photon)
        predSum += prediction
    }

    mutating func addInterval(_ ms: Double) {
        intervalN += 1
        intervalSum += ms
        intervalSq += ms * ms
        intervalMax = max(intervalMax, ms)
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
    private(set) var options: ServerOptions
    private var pose: PoseSource!
    private var renderer: RNSpatialRenderer!
    private var window: NSWindow!
    private var view: MetalView!
    private var displayLink: CAMetalDisplayLink?
    private var renderThread: Thread?
    private var captures: [ScreenCapture] = []
    private var floatingCaptures: [ScreenCapture] = []  // floating windows, panels after the monitors
    private(set) var floatingResolved: [FloatingWindow] = []  // with window ids and poses
    // A monitor of its own per floating window (nil: captured where it is), placed in the
    // macOS arrangement where the window floats, so the cursor reaches it like a monitor.
    private var floatingDisplays: [RNVirtualDisplay?] = []
    private var floatingSerials: [UInt32] = []  // with floatingDisplays (0: none)
    private var floatingArrangeWork: DispatchWorkItem?
    private var desktop: VirtualDesktop?
    private var patternWindows: [NSWindow] = []
    private var panelDisplays: [CGDirectDisplayID] = []  // left -> right
    private var gazeTimer: Timer?
    private var lastGazePanel = -2
    private var gazeCandidate = -2          // panel looked at, not yet followed
    private var gazeCandidateSince = Date()
    private var growCandidate = -2          // pinned: panel looked at, not yet enlarged
    private var growCandidateSince = Date()
    private var growCursorDisplay: CGDirectDisplayID = 0
    // Floating windows on a monitor of their own: only the window's part of it is shown.
    private var floatingCrops: [(capture: ScreenCapture, windowID: CGWindowID, display: CGDirectDisplayID, crop: CGRect)] = []
    private var physicalDisplay: CGDirectDisplayID?  // glasses surface when it is not a panel
    private var glassesID: CGDirectDisplayID?        // render target (nil in --windowed)
    private var lastPoseNs: UInt64 = 0
    private var stalePoseSeconds = 0
    private var hotKeys: HotKeys? = HotKeys()
    private var statusMenu: StatusMenu?
    private var screenObserver: NSObjectProtocol?
    private var stopped = false
    // Keeps macOS from napping or throttling the app while it runs: once another app is in
    // front (a click on a virtual monitor), App Nap and timer coalescing would delay the IMU
    // and render threads and the panels would tremble.
    private var activity: NSObjectProtocol?
    // AI 3D: depth for the cinema panel (DepthEngine); the model loads the first time it is wanted.
    private let depth3D = DepthEngine()
    private var depthLoading: Task<Void, Never>?
    private var depthPanel = -1  // main thread: panel last given depth (to flatten it again)

    // GUI hooks. Without them (CLI) a stop request quits the process.
    var onStopRequest: ((String) -> Void)?
    var onFovChanged: ((Float) -> Void)?
    var onNotice: ((String) -> Void)?  // short messages for the user (a notification in the app)
    private(set) var isTracking = false

    private let statsLock = NSLock()
    private var lastPresentedTime = 0.0  // guarded by statsLock
    private var stats = LatencyStats()
    private var lastCaptureFrames: UInt64 = 0
    private var statsTimer: Timer?
    private var calibSeconds = 0
    private var overlayShown = false  // start-up message on the glasses until tracking starts
    // A short message on the glasses (main thread): until when, its lines, the one waiting for
    // the start-up message to end.
    private var flashWork: DispatchWorkItem?
    private var flashLines: [String] = []
    private var flashAt = Date.distantPast
    private var pendingFlash: [String] = []

    // Idle skip (render thread): with the head still and no monitor changing, the last
    // frame is still correct, so nothing is drawn. Settings changes force one redraw.
    private var redrawRequested = true  // guarded by statsLock
    // How much later than its target a frame actually reaches the glasses (ms, smoothed): the
    // head pose is predicted for that real moment, or the panels lag and jerk while turning.
    private var presentLateMs = 0.0  // guarded by statsLock
    private var lastDrawn: simd_quatf?  // render thread
    private let drawLock = NSLock()     // one frame at a time: display link or fallback loop
    private var linkFrames: UInt64 = 0  // guarded by statsLock: display link callbacks so far
    // Late latching (guarded by statsLock): the display link wakes ~3 frames before the frame
    // is seen; the pose is read at the deadline minus this margin instead, so less of the head's
    // motion has to be predicted. A frame shown late widens it, a long run on time narrows it.
    private var latchMarginS = 0.004
    private var latchOnTime = 0
    private var framePeriodS = 1.0 / 120
    private var fallbackThread: Thread? // main thread: drawing while the display link is stalled

    init(options: ServerOptions) {
        self.options = options
    }

    // Async so the app stays responsive while displays reconfigure and monitors appear: the
    // waits below yield to the main actor instead of spinning a nested run loop. Cancelling
    // the task aborts at the next wait; stop() then tears down whatever was built.
    @MainActor
    func start() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw fail("no Metal device") }
        activity = ProcessInfo.processInfo.beginActivity(
            options: [.userInitiated, .latencyCritical, .idleDisplaySleepDisabled],
            reason: "Nimbo: head tracking and rendering on the glasses")

        // Output: the glasses, or a debug window on the Mac screen. Mirrored by the hardware they
        // are no screen of their own: mirroring off first, then they appear.
        if !options.windowed, Displays.glassesScreen() == nil, let id = Displays.glassesDisplayID() {
            log(Displays.unmirror(glasses: id) ? "mirroring: off (the glasses were a mirror)" : "mirroring: could not disable")
            for _ in 0..<20 where Displays.glassesScreen() == nil { try await settle(0.25) }
        }
        let glasses = Displays.glassesScreen()
        var screen: NSScreen
        if options.windowed {
            screen = NSScreen.main!
        } else if let glasses {
            screen = glasses
        } else {
            throw fail("RayNeo GT display (SmartGlasses) not found. Without the glasses: --windowed --sim")
        }
        let screenID = Displays.id(of: screen)
        if options.unmirror && !options.windowed {
            log(Displays.unmirror(glasses: screenID) ? "mirroring: off (app-only)" : "mirroring: could not disable")
            try await settle(1.5)  // let the reconfiguration settle
        }
        log("output: \(screen.localizedName) id=\(screenID) \(Int(screen.frame.width))x\(Int(screen.frame.height))"
            + (Displays.isMirrored(screenID) ? " [mirrored]" : "") + (options.windowed ? " [window]" : ""))
        if !options.windowed {
            if let m = Displays.setNative(screenID, hz: options.refresh120 ? 120 : 60) {
                log("glasses mode: \(m.pixelWidth)x\(m.pixelHeight) native @\(Int(m.refreshRate)) Hz (your setting is restored on exit)")
                try await settle(1.0)
                if let s = NSScreen.screens.first(where: { Displays.id(of: $0) == screenID }) { screen = s }
            } else {
                log("glasses mode: unchanged")
            }
        }

        // Pose first: the IMU needs ~6 s of rest, let it start while the rest comes up.
        if options.sim {
            pose = SimPoseSource(sweepAmplitudeDeg: options.simSweepDeg)
            log("tracking: simulated head" + (options.simSweepDeg > 0 ? " (sweep ±\(options.simSweepDeg)°)" : " (Cmd+Opt+arrows)"))
        } else {
            let imu = ImuPoseSource(calibrationPath: options.calibrationPath,
                                    calibrationRequired: options.calibrationRequired,
                                    deadZoneDeg: options.deadZoneDeg)
            pose = imu
        }
        try pose.start()
        if let imu = pose as? ImuPoseSource {
            if options.glassesMode >= 0 {
                imu.setGlassesMode(options.glassesMode)  // pinned: centred in front of you
                log("glasses: " + ["follow", "pinned", "steady"][min(2, options.glassesMode)] + " mode")
            }
            if options.brightness > 0 {
                imu.setGlassesBrightness(options.brightness)
                log("glasses: brightness \(options.brightness)")
            }
        }
        if let imu = pose as? ImuPoseSource {
            log("calibration: " + (imu.calibrationLoaded ? options.calibrationPath : "none (identity)"))
            log("imu: streaming, leave the glasses still on the table ~6 s until TRACK")
        }

        // Virtual monitors before the window, so the layout is settled when we draw.
        if options.displays > 0 {
            // The Mac's own screen as panel 1: one virtual monitor fewer.
            let builtIn = options.macScreen && !options.windowed && !options.includeMain ? Displays.builtIn() : nil
            // Next to the Mac's screen, the virtual monitors take its shape (16:10 on a MacBook):
            // at 16:9 the same width drew them shorter, as if cut top and bottom.
            var vdHeight = options.vdHeight
            if let builtIn {
                let b = CGDisplayBounds(builtIn)
                if b.width > 0 { vdHeight = Int((Double(options.vdWidth) * b.height / b.width).rounded()) & ~1 }
            }
            let vd = try VirtualDesktop(count: options.displays - (builtIn != nil ? 1 : 0), width: options.vdWidth,
                                        height: vdHeight, hiDPI: options.hiDPI, refresh: 60)
            desktop = vd
            try await settle(0.5)
            let anchor = builtIn ?? CGMainDisplayID()
            let virtualMain = !options.includeMain && !options.windowed
            macScreenID = builtIn
            let layout = options.layout
            let arrange = { () -> Bool in
                if let builtIn {
                    return vd.arrange(after: builtIn, cells: layout, tucking: screenID)
                } else if virtualMain {
                    return vd.arrangeAsMain(tucking: screenID, cells: layout)
                }
                return vd.arrange(rightOf: anchor, cells: layout)
            }
            if !arrange() { log("displays: macOS refused the arrangement") }
            try await settle(1.0)  // NSScreen list catches up
            // With "mirror displays" on in macOS, new monitors can join the Mac's mirror set: then
            // they sit on top of it (all at 0,0) and cannot be captured, and Nimbo did not start
            // with three monitors. Out of the mirror set and arranged again.
            for _ in 0..<3 {
                let mirrored = ([builtIn].compactMap { $0 } + vd.ids).filter { CGDisplayIsInMirrorSet($0) != 0 }
                if mirrored.isEmpty { break }
                log("displays: \(mirrored.map(String.init).joined(separator: ", ")) mirrored; mirroring off and arranged again")
                var config: CGDisplayConfigRef?
                if CGBeginDisplayConfiguration(&config) == .success {
                    for id in mirrored { CGConfigureDisplayMirrorOfDisplay(config, id, kCGNullDirectDisplay) }
                    CGCompleteDisplayConfiguration(config, .forAppOnly)
                }
                try await settle(0.8)
                if !arrange() { log("displays: macOS refused the arrangement") }
                try await settle(1.0)
            }
            copyWallpaper(to: vd.ids, from: builtIn)
            log("virtual monitors: " + vd.ids.map { id in
                let b = CGDisplayBounds(id)
                return "\(id)@\(Int(b.minX)),\(Int(b.minY)) \(Int(b.width))x\(Int(b.height))"
            }.joined(separator: "  ") + (builtIn != nil ? "  (panel 1 is the Mac's own screen; glasses surface tucked away)"
                                         : virtualMain ? "  (panel 1 is the main display; glasses surface tucked away)"
                                                       : "  (right of display \(anchor))"))
            if virtualMain {
                physicalDisplay = screenID
                let target = CGDisplayBounds(builtIn ?? vd.ids[0])
                CGWarpMouseCursorPosition(CGPoint(x: target.midX, y: target.midY))
                if options.gatherWindows {
                    if WindowGatherer.isTrusted(prompt: true) {
                        let n = WindowGatherer.gather(from: CGDisplayBounds(screenID), into: target)
                        log("windows: moved \(n) window(s) to panel 1")
                    } else {
                        log("windows: allow this app (or Terminal) in System Settings > Privacy & Security > " +
                            "Accessibility to bring your open windows into the panels (new windows open there anyway)")
                    }
                }
            }
            // The main display (your real desktop) sits left of the virtual row, matching
            // where the cursor goes when it leaves it to the right.
            panelDisplays = (options.includeMain || builtIn != nil ? [anchor] : []) + vd.ids
            let offset = options.includeMain || builtIn != nil ? 1 : 0
            if options.testPattern {
                for (i, id) in vd.ids.enumerated() {
                    guard let s = vd.screen(for: id) else { continue }
                    patternWindows.append(TestPatternView.makeWindow(on: s, index: i + offset, total: panelDisplays.count))
                }
            }
        }

        // The arrangement above moved the glasses display: resolve its current frame again,
        // otherwise the render window lands on whatever now occupies the old coordinates.
        if !options.windowed, let moved = NSScreen.screens.first(where: { Displays.id(of: $0) == screenID }) {
            screen = moved
        }
        glassesID = options.windowed ? nil : screenID

        // Render window.
        let rect = options.windowed ? NSRect(x: 80, y: 80, width: 1280, height: 720) : screen.frame
        window = NSWindow(contentRect: rect, styleMask: options.windowed ? [.titled, .miniaturizable] : [.borderless],
                          backing: .buffered, defer: false, screen: screen)
        window.title = "Nimbo (vista previa)"
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
        layer.pixelFormat = .bgra8Unorm_srgb  // linear-light filtering (see RNSpatialRenderer.h)
        layer.framebufferOnly = true
        // Pixel size from the display's current mode: NSScreen.backingScaleFactor can still
        // describe the previous (HiDPI) mode right after the native-mode switch.
        var scale = screen.backingScaleFactor
        if !options.windowed, let mode = CGDisplayCopyDisplayMode(screenID), rect.width > 0 {
            scale = CGFloat(mode.pixelWidth) / rect.width
        }
        layer.contentsScale = scale
        layer.drawableSize = CGSize(width: rect.width * scale, height: rect.height * scale)
        if !options.windowed, let mode = CGDisplayCopyDisplayMode(screenID), mode.refreshRate > 0 {
            framePeriodS = 1 / max(30, mode.refreshRate)
        }
        log(String(format: "surface: window %.0fx%.0f pt, scale %.1f, drawable %.0fx%.0f px",
                   rect.width, rect.height, scale, layer.drawableSize.width, layer.drawableSize.height))
        layer.maximumDrawableCount = 2  // 3 measured identical; 2 holds less queued work
        layer.displaySyncEnabled = true  // without vsync the scanout time does not move, only the estimate

        renderer = try RNSpatialRenderer(device: device, pixelFormat: .bgra8Unorm_srgb)
        // One image per eye when the glasses are in their 3D mode (nothing to switch in Nimbo).
        let sbs = options.sideBySide ?? autoSideBySide
        let ro = renderer.options
        ro.sideBySide = sbs
        ro.ipdMeters = options.ipdMM / 1000
        ro.distanceMeters = options.distanceM
        ro.arcDegrees = options.panelArcDeg
        ro.gapDegrees = options.gapDeg
        ro.curved = options.curved
        ro.curvature = options.curvature
        ro.tilt = options.tilt
        ro.fovYDegrees = options.fovYDeg
        if !options.fovFromCLI, SettingsFile.shared.object(forKey: "fovY") != nil {
            ro.fovYDegrees = SettingsFile.shared.float(forKey: "fovY")
        }
        ro.k1 = options.k1
        ro.k2 = options.k2
        ro.dimOthers = options.dimOthers
        ro.trackingAxes = options.trackingAxes
        ro.depthStrength = options.depthStrength
        ro.depthConvergence = options.depthConvergence
        depth3D.allowGPU = options.depthGPU
        depth3D.onFrame = { [weak self] color, depth, low, high, cut, panel in
            self?.renderer?.setSourcePixelBuffer(color, depth: depth, low: low, high: high, sceneCut: cut, forPanel: UInt(panel))
        }
        let panels = max(1, panelDisplays.count)
        renderer.panelCount = UInt(panels)
        if let layout = options.layout, layout.count == panels {
            renderer.setGridColumns(layout.map { NSNumber(value: $0.col) }, rows: layout.map { NSNumber(value: $0.row) })
        }
        applyPoses(options)
        if options.fit && options.arcDeg == nil {
            let columns = Set((options.layout ?? []).map(\.col)).count
            ro.arcDegrees = renderer.fitArcDegrees(forPanels: UInt(max(columns, options.layout == nil ? panels : 1)))
        }
        applyPinned(options)
        log("tracking: " + (options.glassesPinned ? "by the glasses (pinned mode); panels fitted inside their image" : ["full head rotation", "left/right turn only (panels never move vertically)",
                            "up/down only (panels never move sideways)"][max(0, min(2, ro.trackingAxes))]))
        log("render: \(sbs ? "side by side (glasses in 3D)" : "mono"), \(panels) \(options.curved ? "curved" : "flat") panel(s) of \(String(format: "%.1f", ro.arcDegrees))° gap \(options.gapDeg)° at \(options.distanceM) m, ipd \(options.ipdMM) mm, fov \(ro.fovYDegrees)°, k1 \(options.k1) k2 \(options.k2)")

        registerHotKeys()
        if options.headless { statusMenu = StatusMenu { [weak self] in self?.recenter() } }
        // The hardware cursor only needs hiding when the glasses mirror the captured display (one
        // panel, no virtual monitors): then it is drawn head-locked over the render. With virtual
        // monitors the gaze timer keeps it off the glasses surface; hidden, it also vanished from
        // the panels whenever Nimbo itself was the active app (its panel, "Más opciones").
        if !options.windowed, options.displays == 0, Displays.isMirrored(screenID) {
            log(HardwareCursor.hide() ? "cursor: hardware cursor hidden (mirrored glasses; the panels show the real one)"
                                      : "cursor: could not hide the hardware cursor")
        }
        if pose is ImuPoseSource {  // the IMU calibrates first: say so on the glasses
            renderer.setOverlayText("Nimbo se está preparando\nDeja las gafas quietas unos segundos")
            overlayShown = true
        }
        try await startCaptures(glasses: glasses)
        await setFloatingWindows(options.floatingWindows)
        if options.startFocusPanel > 0, let ro = renderer?.options {
            ro.focusPanel = options.startFocusPanel - 1
            focusMode = options.startCinema ? 1 : 2  // toggled into the wanted mode below
            toggleFocus(cinema: options.startCinema)
        }
        try Task.checkCancellation()
        startRenderLoop(layer: layer)
        startMonitoring()
    }

    // Plain (non-isolated) context: these main-queue timers capture self like the rest of the class.
    private func startMonitoring() {
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.printStats()
            self?.checkGlassesPresent()
            self?.refreshFloatingCrops()
        }
        screenObserver = NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification,
                                                                object: nil, queue: .main) { [weak self] _ in
            self?.screensChanged()
        }
        // Always: the gaze also picks the enlarged panel in pinned, which can be switched on later.
        gazeTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            self?.followGaze()
        }
        if (options.gazeCursor && (panelDisplays.count > 1 || !floatingDisplays.isEmpty)) || physicalDisplay != nil {
            log("cursor: follows the panel you look at (--no-gaze-cursor to disable)")
        }
    }

    // MARK: glasses presence

    // Keeps the render window on the glasses display whatever macOS does with the layout.
    private func screensChanged() {
        guard let id = glassesID else { return }
        guard let s = NSScreen.screens.first(where: { Displays.id(of: $0) == id }) else {
            glassesLost()
            return
        }
        if window.frame != s.frame { window.setFrame(s.frame, display: true) }
        fitSurface(to: s)
        // A monitor coming or going (a floating window added or removed) and macOS lays the
        // displays out its own way: the glasses' surface ended up beside the Mac's screen (the
        // cursor fell onto it and jumped back) and a floating window's monitor to the right.
        // Ours again, once things settle; our own changes do not count.
        guard Date() > arrangingUntil else { return }
        rearrangeWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.rearrangeDisplays() }
        rearrangeWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8, execute: work)
    }

    // New virtual monitors start with macOS's default wallpaper (or none): give them yours,
    // from the Mac's own screen (else the first screen that has one).
    private func copyWallpaper(to ids: [CGDirectDisplayID], from source: CGDirectDisplayID?) {
        let ws = NSWorkspace.shared
        let mine = Set(ids)
        let from = NSScreen.screens.first { source != nil && Displays.id(of: $0) == source }
            ?? NSScreen.screens.first { !mine.contains(Displays.id(of: $0)) && Displays.glassesScreen() != $0 }
        guard let from, let url = ws.desktopImageURL(for: from) else { return }
        let options = ws.desktopImageOptions(for: from) ?? [:]
        var done = 0
        for screen in NSScreen.screens where mine.contains(Displays.id(of: screen)) {
            if (try? ws.setDesktopImageURL(url, for: screen, options: options)) != nil { done += 1 }
        }
        log("wallpaper: \(url.lastPathComponent) on \(done) monitor(s)")
    }

    private var macScreenID: CGDirectDisplayID?   // the Mac's own screen as panel 1
    private var arrangingUntil = Date.distantPast  // our own reconfiguration's notifications
    private var rearrangeWork: DispatchWorkItem?

    // The monitors as their grid cells, the floating windows' monitors where they float, and
    // everything else (the glasses' surface, the Mac's screen when it is not a panel) tucked
    // below-left.
    private func rearrangeDisplays() {
        guard let vd = desktop, let glasses = glassesID, physicalDisplay != nil, !stopped else { return }
        vd.keep = floatingDisplays.compactMap { $0?.displayID }
        arrangingUntil = Date().addingTimeInterval(1.5)
        if let mac = macScreenID {
            vd.arrange(after: mac, cells: options.layout, tucking: glasses)
        } else {
            vd.arrangeAsMain(tucking: glasses, cells: options.layout)
        }
        arrangeFloatingDisplays()
        log("displays: arrangement restored")
    }

    // Unplugged glasses: the display disappears and/or the IMU stops. Quit, which restores
    // the original display arrangement (all display changes are app-scoped).
    private func checkGlassesPresent() {
        if let id = glassesID, CGDisplayIsOnline(id) == 0 || CGDisplayIsActive(id) == 0 && CGDisplayIsInMirrorSet(id) == 0 {
            glassesLost()
        } else {
            glassesLostSince = nil
        }
        guard pose is ImuPoseSource, let p = pose.latest() else { return }
        stalePoseSeconds = p.hostTimeNs == lastPoseNs ? stalePoseSeconds + 1 : 0
        lastPoseNs = p.hostTimeNs
        if stalePoseSeconds >= 2 { shutDown("glasses IMU stopped (unplugged?)") }
    }

    // MARK: glasses 2D / 3D

    // In --windowed: mono; on the glasses: side by side when they are in their 3D mode.
    private var autoSideBySide: Bool { glassesID.map(Displays.isSideBySide) ?? false }

    // The glasses' display went away. Switching them between 2D and 3D does that for a moment
    // (and comes back at another size) while the IMU keeps streaming: wait for it rather than
    // stop. Unplugged, the IMU stops too and the check above stops Nimbo.
    private var glassesLostSince: Date?

    private var reconnecting = false  // main thread: setting the glasses' display up again
    private var cinemaFor3D = false   // cinema entered by ⌘⌥T
    private var lastUnmirror = Date.distantPast

    private func glassesLost() {
        guard !reconnecting else { return }
        if Displays.glassesScreen() == nil, let id = Displays.glassesDisplayID(), Date() > arrangingUntil {
            arrangingUntil = Date().addingTimeInterval(2)
            log("glasses: back as a mirror; mirroring off")
            _ = Displays.unmirror(glasses: id)  // they become a screen of their own: next check picks them
        }
        if let s = Displays.glassesScreen(), CGDisplayIsOnline(Displays.id(of: s)) != 0 {
            glassesLostSince = nil
            reconnecting = true
            Task { @MainActor in await self.retargetGlasses(s) }
            return
        }
        if glassesLostSince == nil {
            glassesLostSince = Date()
            log("glasses display gone (switching 2D / 3D?): waiting for it")
        } else if let since = glassesLostSince, Date().timeIntervalSince(since) > 10 {
            shutDown("glasses display disconnected")
        }
    }

    // The glasses' display is back (another id or size): set up again as at start, with the
    // same pauses (macOS mirrors it again on its own a moment after it appears, and changes
    // made before that are undone): mirroring off, native mode, the window, its surface and
    // side by side, the display link, the arrangement, the captures.
    @MainActor
    private func retargetGlasses(_ found: NSScreen) async {
        defer { reconnecting = false }
        arrangingUntil = Date().addingTimeInterval(8)  // the gaze cursor waits too
        let id = Displays.id(of: found)
        glassesID = id
        if physicalDisplay != nil { physicalDisplay = id }
        try? await Task.sleep(nanoseconds: 1_500_000_000)
        if stopped { return }
        if options.unmirror {
            log(Displays.unmirror(glasses: id) ? "mirroring: off again" : "mirroring: could not disable")
            try? await Task.sleep(nanoseconds: 1_500_000_000)
        }
        if let m = Displays.setNative(id, hz: options.refresh120 ? 120 : 60) {
            log("glasses mode: \(m.pixelWidth)x\(m.pixelHeight) native @\(Int(m.refreshRate)) Hz")
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
        if stopped { return }
        rearrangeDisplays()
        try? await Task.sleep(nanoseconds: 1_000_000_000)
        let screen = NSScreen.screens.first { Displays.id(of: $0) == id } ?? found
        window.setFrame(screen.frame, display: true)
        window.orderFrontRegardless()
        fitSurface(to: screen)
        // The display link stops with the display that went away.
        displayLink?.invalidate()
        renderThread?.cancel()
        startRenderLoop(layer: view.metalLayer)
        // Display captures stop with a reconfiguration like this one: started afresh.
        let old = captures
        captures = []
        for c in old { await c.stop() }
        do {
            try await startCaptures(glasses: screen)
        } catch {
            log("capture: could not restart: \(error.localizedDescription)")
        }
        arrangingUntil = Date()
        log("glasses: set up again")
    }

    // Drawable at the display's pixel size; side by side with the glasses in 3D.
    private func fitSurface(to screen: NSScreen) {
        guard let id = glassesID, let view, let mode = CGDisplayCopyDisplayMode(id), screen.frame.width > 0 else { return }
        let layer = view.metalLayer
        let size = CGSize(width: mode.pixelWidth, height: mode.pixelHeight)
        let sbs = options.sideBySide ?? Displays.isSideBySide(id)
        guard layer.drawableSize != size || renderer.options.sideBySide != sbs else { return }
        layer.contentsScale = CGFloat(mode.pixelWidth) / screen.frame.width
        layer.drawableSize = size
        statsLock.lock()
        framePeriodS = 1 / max(30, mode.refreshRate)
        statsLock.unlock()
        renderer.options.sideBySide = sbs
        renderer.invalidateSkippedFrame()
        log(String(format: "surface: %.0fx%.0f px, %@", size.width, size.height, sbs ? "side by side (glasses in 3D)" : "mono (glasses in 2D)"))
        onNotice?(sbs ? "Gafas en 3D: una imagen para cada ojo" : "Gafas en 2D")
        flash(sbs ? "Modo 3D de las gafas activado" : "Gafas en modo 2D")
        updateDepth3D()
        requestRedraw()
    }

    // A card in front of the eyes for a few seconds (the start-up message's), e.g. "Modo 3D de
    // las gafas activado". Two messages in a row share it, one per line.
    private func flash(_ line: String, seconds: Double = 2.5) {
        guard let renderer else { return }
        if overlayShown {  // the start-up message is up: after it
            pendingFlash = Array((pendingFlash + [line]).suffix(2))
            return
        }
        flashLines = Date().timeIntervalSince(flashAt) < 0.6 ? Array((flashLines + [line]).suffix(2)) : [line]
        flashAt = Date()
        renderer.setOverlayText(flashLines.joined(separator: "\n"))
        renderer.invalidateSkippedFrame()
        requestRedraw()
        flashWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, !self.overlayShown else { return }
            self.renderer?.setOverlayText(nil)
            self.requestRedraw()
        }
        flashWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds, execute: work)
    }

    // For the app's own shortcuts (⌘⌥K...): the same card on the glasses.
    func showMessage(_ text: String) { flash(text) }

    private func shutDown(_ reason: String) {
        log("\(reason): stopping and restoring the displays")
        requestStop(reason)
    }

    private func requestStop(_ reason: String) {
        if let onStopRequest { onStopRequest(reason) } else { NSApp.terminate(nil) }
    }

    // MARK: live settings

    // Applies what the renderer can change without rebuilding monitors or captures.
    func applyLive(_ o: ServerOptions) {
        guard let ro = renderer?.options else { return }
        ro.distanceMeters = o.distanceM
        ro.gapDegrees = o.gapDeg
        ro.curved = o.curved
        ro.curvature = o.curvature
        ro.tilt = o.tilt
        ro.fovYDegrees = o.fovYDeg
        ro.ipdMeters = o.ipdMM / 1000
        ro.sideBySide = o.sideBySide ?? autoSideBySide
        ro.k1 = o.k1
        ro.k2 = o.k2
        ro.dimOthers = o.dimOthers
        ro.trackingAxes = o.trackingAxes
        ro.depthStrength = o.depthStrength
        ro.depthConvergence = o.depthConvergence
        if let arc = o.arcDeg { ro.arcDegrees = arc }
        applyPoses(o)
        options.distanceM = o.distanceM
        options.gapDeg = o.gapDeg
        options.curved = o.curved
        options.curvature = o.curvature
        options.tilt = o.tilt
        options.arcDeg = o.arcDeg
        options.fovYDeg = o.fovYDeg
        options.ipdMM = o.ipdMM
        options.sideBySide = o.sideBySide
        options.k1 = o.k1
        options.k2 = o.k2
        options.gazeCursor = o.gazeCursor
        options.dimOthers = o.dimOthers
        options.trackingAxes = o.trackingAxes
        options.panelPoses = o.panelPoses
        if let imu = pose as? ImuPoseSource {
            if o.glassesMode >= 0 && o.glassesMode != options.glassesMode { imu.setGlassesMode(o.glassesMode) }
            if o.brightness > 0 && o.brightness != options.brightness { imu.setGlassesBrightness(o.brightness) }
        }
        options.glassesMode = o.glassesMode
        options.recenterGlasses = o.recenterGlasses
        options.brightness = o.brightness
        options.glassesPinned = o.glassesPinned
        options.growFocused = o.growFocused
        options.realSize = o.realSize
        options.focusWindows = o.focusWindows
        options.depth3D = o.depth3D
        options.depthStrength = o.depthStrength
        options.depthConvergence = o.depthConvergence
        applyPinned(o)  // last: it overrides the panel width
        updateDepth3D()
        requestRedraw()
    }

    // MARK: floating windows

    // ⌘⌥J on a floating window: its real window comes to the front with the cursor on it, to
    // pause the video or type without looking for it.
    private func focusFloatingWindow(_ w: FloatingWindow) {
        guard let list = CGWindowListCopyWindowInfo([.optionIncludingWindow], CGWindowID(w.windowID)) as? [[String: Any]],
              let info = list.first, let pid = info[kCGWindowOwnerPID as String] as? pid_t else { return }
        NSRunningApplication(processIdentifier: pid)?.activate()
        // Raise the right window of that app (Accessibility; without it the app's front window).
        let axApp = AXUIElementCreateApplication(pid)
        var value: CFTypeRef?
        if AXUIElementCopyAttributeValue(axApp, kAXWindowsAttribute as CFString, &value) == .success,
           let windows = value as? [AXUIElement] {
            let match = windows.first { win in
                var t: CFTypeRef?
                return AXUIElementCopyAttributeValue(win, kAXTitleAttribute as CFString, &t) == .success && (t as? String) == w.windowTitle
            } ?? windows.first
            if let match { AXUIElementPerformAction(match, kAXRaiseAction as CFString) }
        }
        if let b = info[kCGWindowBounds as String] as? [String: CGFloat], let x = b["X"], let y = b["Y"],
           let width = b["Width"], let height = b["Height"] {
            CGWarpMouseCursorPosition(CGPoint(x: x + width / 2, y: y + height / 2))
            CGAssociateMouseAndMouseCursorPosition(1)
        }
        log("window: focus \(w.title)")
    }

    private var monitorPanels: Int { max(1, panelDisplays.count) }

    // Where a new floating window goes: centred above the monitors, one monitor wide.
    private func defaultFloatingPose(above index: Int) -> PanelPos {
        let size = options.panelArcDeg, gap = options.gapDeg
        let vArc = 2 * atan(tan(size * .pi / 360) / (16.0 / 9.0)) * 180 / .pi
        let top: Float
        if let poses = options.panelPoses, !poses.isEmpty {
            top = poses.map { $0.pitch + vArc * $0.scale / 2 }.max() ?? vArc / 2
        } else {
            let rows = Set((options.layout ?? []).map(\.row)).count
            top = Float(max(1, rows) - 1) * (vArc + gap) / 2 + vArc / 2
        }
        return PanelPos(yaw: 0, pitch: top + gap + vArc / 2 + Float(index) * (vArc + gap), scale: 1)
    }

    // Starts (or restarts) the captures of the floating windows and makes them panels after
    // the monitors. Windows given by text are looked up by title / app name. With virtual
    // monitors and Accessibility, each window moves to a monitor of its own (see
    // floatingDisplays); otherwise it is captured wherever it is.
    @MainActor
    func setFloatingWindows(_ specs: [FloatingWindow]) async {
        let old = floatingCaptures
        floatingCaptures = []
        floatingCrops = []
        for c in old { await c.stop() }
        // Monitors of the windows already afloat, by window: kept for the ones that stay.
        var kept: [UInt32: (display: RNVirtualDisplay, serial: UInt32)] = [:]
        for (i, w) in floatingResolved.enumerated() where i < floatingDisplays.count {
            if let d = floatingDisplays[i] { kept[w.windowID] = (d, floatingSerials[i]) }
        }
        let oldDisplays = floatingDisplays  // released last, once the windows leaving are back on monitor 1
        defer { withExtendedLifetime(oldDisplays) {} }
        var resolved: [FloatingWindow] = []
        let base = monitorPanels
        for spec in specs.prefix(8 - base) {
            var w = spec
            // A window id from an earlier session is gone: find it again by app and title.
            let known = await ScreenCapture.windows()
            if w.windowID != 0, !known.contains(where: { $0.id == w.windowID }) { w.windowID = 0 }
            if w.windowID == 0 {
                let found = !w.app.isEmpty ? await ScreenCapture.window(app: w.app, title: w.windowTitle)
                                            : await ScreenCapture.window(matching: spec.match)
                guard let found else {
                    log("window: \(w.app.isEmpty ? "nothing matches \"\(spec.match)\"" : "\(w.app) is not open")")
                    continue
                }
                w.windowID = found.id
            }
            if let info = known.first(where: { $0.id == w.windowID }) {
                w.app = info.app
                w.windowTitle = info.title
                w.title = info.app + (info.title.isEmpty ? "" : " — " + info.title)
            }
            if w.pose == nil { w.pose = defaultFloatingPose(above: resolved.count) }
            resolved.append(w)
        }

        // Windows no longer afloat leave their monitor before it disappears: back to monitor 1,
        // at their size (macOS would otherwise drop them anywhere, even on the hidden surface).
        let staying = Set(resolved.map(\.windowID))
        for w in floatingResolved where kept[w.windowID] != nil && !staying.contains(w.windowID) {
            returnToFirstMonitor(w)
        }

        // Monitors of their own: the same one for a window that stays, a new one for a new window.
        var displays = [RNVirtualDisplay?](repeating: nil, count: resolved.count)
        var serials = [UInt32](repeating: 0, count: resolved.count)
        var fresh: [Int] = []
        let ownMonitors = desktop != nil && !options.windowed && !resolved.isEmpty
        if ownMonitors && !WindowGatherer.isTrusted(prompt: false) {
            log("window: allow Nimbo in Privacy & Security > Accessibility so the cursor can reach floating windows")
        } else if ownMonitors {
            for (i, w) in resolved.enumerated() {
                if let k = kept[w.windowID] {
                    displays[i] = k.display
                    serials[i] = k.serial
                }
            }
            for (i, w) in resolved.enumerated() where displays[i] == nil {
                guard let frame = WindowGatherer.frame(windowID: CGWindowID(w.windowID)) else { continue }
                let width = Int(max(640, min(2560, frame.width))) & ~7
                let height = Int(max(360, min(1440, frame.height))) & ~7
                // A serial no live monitor has (the old ones still exist until the end).
                let geometry = UInt32(0x8000_0000) | UInt32((width / 16) << 16 | (height / 16) << 4)
                let used = Set(serials + floatingSerials)
                guard let serial = (0..<8).map({ geometry | UInt32($0) }).first(where: { !used.contains($0) }) else { continue }
                do {
                    displays[i] = try RNVirtualDisplay(name: "Nimbo · \(w.app)", width: UInt(width), height: UInt(height),
                                                       refreshRate: 60, hiDPI: options.hiDPI, serial: serial)
                    serials[i] = serial
                    fresh.append(i)
                } catch {
                    log("window: no monitor of its own for \(w.title): \(error.localizedDescription)")
                }
            }
        }
        floatingDisplays = displays
        floatingSerials = serials
        floatingResolved = resolved
        if !fresh.isEmpty {
            try? await settle(1.0)  // NSScreen list catches up
            arrangeFloatingDisplays()
            try? await settle(0.5)
            for i in fresh {
                guard let d = displays[i] else { continue }
                let w = resolved[i]
                let target = Displays.visibleBounds(d.displayID) ?? CGDisplayBounds(d.displayID)
                if !WindowGatherer.move(windowID: CGWindowID(w.windowID), title: w.windowTitle, to: target) {
                    log("window: could not move \(w.title) to its monitor")
                }
            }
            try? await settle(0.3)
        }

        for (i, w) in resolved.enumerated() {
            let panel = UInt(base + floatingCaptures.count)
            let cap = ScreenCapture { [weak self] pb in self?.deliver(pb, panel: Int(panel)) }
            do {
                let (pw, ph): (Int, Int)
                if let d = displays[i] {  // the window's part of its monitor, with the cursor over it
                    let crop = windowCrop(CGWindowID(w.windowID), on: d.displayID)
                    (pw, ph) = try await cap.start(displayID: d.displayID, fps: 60, excludeOwnApp: true,
                                                   minScale: options.hiDPI ? 2 : 1, crop: crop)
                    if let crop { floatingCrops.append((cap, CGWindowID(w.windowID), d.displayID, crop)) }
                } else {
                    (pw, ph) = try await cap.start(windowID: CGWindowID(w.windowID), fps: 60)
                }
                floatingCaptures.append(cap)
                log("window: panel \(Int(panel) + 1) <- \(w.title) \(pw)x\(ph)" + (displays[i].map { " on monitor \($0.displayID)" } ?? ""))
            } catch {
                log("window: \(error.localizedDescription)")
            }
        }
        floatingResolved = resolved
        options.floatingWindows = resolved
        // The enlarged panel was a window: the windows' panels may have shifted.
        if renderer.growPanel >= base { renderer.growPanel = -1 }
        renderer.panelCount = UInt(base + resolved.count)
        applyFloatingPoses(resolved.map { $0.pose ?? PanelPos(yaw: 0, pitch: 0) })
    }

    // Moving or resizing floating windows (editor), live; their monitors follow once the
    // dragging stops.
    func applyFloatingPoses(_ poses: [PanelPos]) {
        guard let renderer else { return }
        for (i, p) in poses.enumerated() where i < floatingResolved.count { floatingResolved[i].pose = p }
        renderer.setFloatingPosesYaw(poses.map { NSNumber(value: $0.yaw) }, pitch: poses.map { NSNumber(value: $0.pitch) },
                                     scale: poses.map { NSNumber(value: $0.scale) })
        requestRedraw()
        floatingArrangeWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.arrangeFloatingDisplays() }
        floatingArrangeWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6, execute: work)
    }

    // A window leaving its monitor of its own: centred on monitor 1 at its size (no larger than
    // the room there).
    private func returnToFirstMonitor(_ w: FloatingWindow) {
        guard let first = panelDisplays.first, let frame = WindowGatherer.frame(windowID: CGWindowID(w.windowID)) else { return }
        let room = Displays.visibleBounds(first) ?? CGDisplayBounds(first)
        let size = CGSize(width: min(frame.width, room.width), height: min(frame.height, room.height))
        let target = CGRect(x: room.midX - size.width / 2, y: room.midY - size.height / 2,
                            width: size.width, height: size.height).integral
        if WindowGatherer.move(windowID: CGWindowID(w.windowID), title: w.windowTitle, to: target) {
            log("window: \(w.title) back on monitor 1")
        } else {
            log("window: could not bring \(w.title) back to monitor 1")
        }
    }

    // The window's frame on its monitor (points, the monitor's top-left), so the panel shows the
    // window and not the monitor around it. nil: not on it.
    private func windowCrop(_ windowID: CGWindowID, on display: CGDirectDisplayID) -> CGRect? {
        guard let frame = WindowGatherer.frame(windowID: windowID) else { return nil }
        let b = CGDisplayBounds(display)
        let r = frame.intersection(b).offsetBy(dx: -b.minX, dy: -b.minY).integral
        return r.width >= 64 && r.height >= 64 ? r : nil
    }

    // Once a second: a floating window moved or resized on its monitor, the crop follows.
    private func refreshFloatingCrops() {
        for (i, f) in floatingCrops.enumerated() {
            guard let crop = windowCrop(f.windowID, on: f.display), crop != f.crop else { continue }
            floatingCrops[i].crop = crop
            let cap = f.capture
            Task { await cap.updateCrop(crop) }
        }
    }

    // A monitor's pose in degrees (yaw right, pitch up): hand-placed or its grid cell.
    private func monitorPose(_ i: Int) -> PanelPos {
        if let poses = options.panelPoses, poses.count == panelDisplays.count, poses.indices.contains(i) { return poses[i] }
        let size = options.panelArcDeg, gap = options.gapDeg
        let vArc = 2 * atan(tan(size * .pi / 360) / (16.0 / 9.0)) * 180 / .pi
        guard let cells = options.layout, cells.count == panelDisplays.count, cells.indices.contains(i) else {
            return PanelPos(yaw: Float(i) * (size + gap), pitch: 0, scale: 1)
        }
        return PanelPos(yaw: Float(cells[i].col - cells[0].col) * (size + gap),
                        pitch: -Float(cells[i].row - cells[0].row) * (vArc + gap), scale: 1)
    }

    // Each floating window's monitor goes where the window floats relative to monitor 1, at
    // monitor 1's points per degree: above the monitors, the cursor crosses up into it.
    // macOS then snaps it against its neighbours.
    private func arrangeFloatingDisplays() {
        guard let first = panelDisplays.first, floatingDisplays.contains(where: { $0 != nil }) else { return }
        let size = options.panelArcDeg
        let vArc = 2 * atan(tan(size * .pi / 360) / (16.0 / 9.0)) * 180 / .pi
        let b0 = CGDisplayBounds(first), m0 = monitorPose(0)
        let ppdX = b0.width / CGFloat(size * m0.scale), ppdY = b0.height / CGFloat(vArc * m0.scale)
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return }
        for (i, d) in floatingDisplays.enumerated() {
            guard let d, i < floatingResolved.count, let p = floatingResolved[i].pose else { continue }
            let b = CGDisplayBounds(d.displayID)
            let cx = b0.midX + CGFloat(p.yaw - m0.yaw) * ppdX, cy = b0.midY - CGFloat(p.pitch - m0.pitch) * ppdY
            CGConfigureDisplayOrigin(config, d.displayID, Int32(cx - b.width / 2), Int32(cy - b.height / 2))
        }
        if CGCompleteDisplayConfiguration(config, .forAppOnly) != .success {
            log("window: could not place the floating windows' monitors")
        }
    }

    // Display behind a panel, monitors first, then floating windows (0: none of its own).
    private var gazeDisplays: [CGDirectDisplayID] {
        panelDisplays + floatingDisplays.map { $0?.displayID ?? 0 }
    }

    // Hand-placed monitors (layout editor), live.
    private func applyPoses(_ o: ServerOptions) {
        guard let renderer else { return }
        renderer.options.layoutArcDegrees = o.panelArcDeg
        let poses = o.panelPoses ?? []
        renderer.setPanelPosesYaw(poses.map { NSNumber(value: $0.yaw) }, pitch: poses.map { NSNumber(value: $0.pitch) },
                                  scale: poses.map { NSNumber(value: $0.scale) })
        requestRedraw()
    }

    // The glasses' pinned mode anchors the whole image in space and lets the eyes roam over
    // it: moving the panels with the head as well would push them out of that image (the
    // right one ended half cut). So they stay put and are sized to fit side by side in it.
    private func applyPinned(_ o: ServerOptions) {
        guard let renderer else { return }
        let ro = renderer.options
        ro.staticHead = o.glassesPinned  // the renderer fits everything in the glasses' image
        // Which panel you look at: the image is anchored on both axes, so a window above is
        // reached by looking up.
        if o.glassesPinned { ro.trackingAxes = 0 }
        if !(o.glassesPinned && o.growFocused) { renderer.growPanel = -1 }
        // Space at real size (glasses in follow): every panel nearly as large as the glasses' own
        // image, fixed in the room on both axes; turning the head brings the next one in. It
        // must fit both ways: the Mac's own screen (3024x1964) is taller than the image, and at
        // the image's full width its top and bottom were cut. 94 % of what fits, clear of the
        // lenses' soft edge. The arc is set so the distance scaling cancels out.
        // Fixed in the room on see-through glasses: the eyes turn about the neck, not about
        // themselves (core::view_matrix).
        ro.neckModel = !o.glassesPinned && o.realSize
        if !o.glassesPinned && o.realSize {
            ro.trackingAxes = 0  // full rotation: fixed in the room, level with the floor
            let fovY = ro.fovYDegrees * .pi / 180
            let tallest = panelDisplays.map { CGDisplayBounds($0) }.filter { $0.height > 0 }
                .map { Float($0.width / $0.height) }.min() ?? ro.eyeAspect
            let wide = 2 * atan(tan(fovY / 2) * ro.eyeAspect)   // the image's width
            let tall = 2 * atan(tan(fovY / 2) * tallest)        // width at which the tallest panel fills its height
            // Flat: exactly the image (1:1 pixels looking straight at it, the sharpest). Curved:
            // 94 %, its edges come closer and would leave the image.
            let fw = (ro.curvature > 0.01 || ro.curved ? 0.94 : 1.0) * min(wide, tall)
            ro.arcDegrees = 2 * atan(tan(fw / 2) * ro.distanceMeters / 2) * 180 / .pi
        }
    }

    // Pinned: the panel in use fills the glasses' image. It is the one you look at for a second
    // (the others lie just beyond the image's edges: look there), or the one you take the mouse
    // to, at once. Floating windows only when allowed.
    private func updateGrow() {
        guard options.glassesPinned, options.growFocused, focusMode == 0, renderer.panelCount > 1,
              let p = pose.latest(), p.tracking else {
            growCandidate = -2
            return
        }
        let current = renderer.growPanel
        if current >= monitorPanels && !options.focusWindows { renderer.growPanel = -1 }
        let cursorPanel = self.cursorPanel()
        let id = cursorPanel.map { gazeDisplays[$0] } ?? 0
        if id != growCursorDisplay {
            growCursorDisplay = id
            let moved = [CGEventType.mouseMoved, .leftMouseDragged, .rightMouseDragged]
                .map { CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: $0) }.min() ?? .infinity
            if let panel = cursorPanel, panel != current, eligible(panel), moved < 0.5 {
                grow(panel, "mouse")
                return
            }
        }
        let panel = renderer.panel(atGaze: p.orientation)
        guard panel >= 0, panel != current, eligible(panel) else {
            growCandidate = -2
            return
        }
        if panel != growCandidate {
            growCandidate = panel
            growCandidateSince = Date()
        } else if Date().timeIntervalSince(growCandidateSince) >= 1.0 {
            grow(panel, "gaze")
        }
    }

    private func grow(_ panel: Int, _ why: String) {
        renderer.growPanel = panel
        growCandidate = -2
        log("grow: panel \(panel + 1) (\(why))")
    }

    // A floating window may be the enlarged / focused panel only when allowed.
    private func eligible(_ panel: Int) -> Bool { panel < monitorPanels || options.focusWindows }

    // Panel whose display has the cursor (monitors, then floating windows on a monitor of their own).
    private func cursorPanel() -> Int? {
        guard let cursor = CGEvent(source: nil)?.location else { return nil }
        let displays = gazeDisplays
        return displays.firstIndex { $0 != 0 && CGDisplayBounds($0).contains(cursor) }
    }

    private func requestRedraw() {
        statsLock.lock()
        redrawRequested = true
        statsLock.unlock()
    }

    // True when the frame about to be drawn would equal the one on screen (core::FrameSkipper).
    private func nothingChanged(_ p: PoseSample) -> Bool {
        statsLock.lock()
        let forced = redrawRequested
        redrawRequested = false
        statsLock.unlock()
        return renderer.canSkipFrame(withOrientation: p.orientation, angularVelocity: p.angularVelocity,
                                     tracking: p.tracking, forced: forced)
    }

    // MARK: gaze cursor

    // When the gaze moves to another panel, bring the cursor along, keeping its relative
    // position (the main display is invisible behind the glasses image otherwise).
    private func followGaze() {
        logCursorState()
        updateGrow()
        // The glasses surface is head-locked: a cursor there would be a second, floating one.
        if let physical = physicalDisplay, !reconnecting, let cursor = CGEvent(source: nil)?.location,
           CGDisplayBounds(physical).contains(cursor) {
            let gaze = pose.latest().map { renderer.panel(atGaze: $0.orientation) } ?? 0
            let b = CGDisplayBounds(panelDisplays[max(0, min(panelDisplays.count - 1, gaze))])
            // The panel's display over the glasses' (macOS mirrored them again): moving the cursor
            // there leaves it on the glasses. Mirroring off once more, every few seconds at most.
            if b.intersects(CGDisplayBounds(physical)) {
                if Date().timeIntervalSince(lastUnmirror) > 3 {
                    lastUnmirror = Date()
                    log("cursor: the glasses mirror a panel again; mirroring off and displays arranged")
                    _ = Displays.unmirror(glasses: physical)
                    rearrangeDisplays()
                }
                return
            }
            log("cursor: on the glasses surface at \(Int(cursor.x)),\(Int(cursor.y)) -> panel \(gaze + 1)")
            CGWarpMouseCursorPosition(CGPoint(x: b.midX, y: b.midY))
            CGAssociateMouseAndMouseCursorPosition(1)
            return
        }
        // --windowed: the cursor is on the Mac's own screen, the panels are only a preview.
        // Focus / cinema: a single panel is shown, the cursor stays put.
        guard focusMode == 0, options.gazeCursor, !options.windowed, let p = pose.latest(), p.tracking else { return }
        let panel = renderer.panel(atGaze: p.orientation)
        guard panel != lastGazePanel else {
            gazeCandidate = panel
            return
        }
        // A glance or the edge between two panels is not a decision: the new panel must hold
        // the gaze for a moment.
        if panel != gazeCandidate {
            gazeCandidate = panel
            gazeCandidateSince = Date()
            return
        }
        if Date().timeIntervalSince(gazeCandidateSince) < 0.8 { return }
        // The mouse and the keyboard win: right after a click, while typing or moving the mouse,
        // the cursor stays where the user is working.
        let since = { (t: CGEventType) in CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: t) }
        let mouseIdle = [CGEventType.mouseMoved, .leftMouseDragged, .rightMouseDragged, .scrollWheel].map(since).min() ?? .infinity
        let clickIdle = [CGEventType.leftMouseDown, .rightMouseDown].map(since).min() ?? .infinity
        if mouseIdle < 2.0 || clickIdle < 5.0 || since(.keyDown) < 3.0 { return }  // re-evaluated next tick
        lastGazePanel = panel
        let displays = gazeDisplays
        guard panel >= 0, panel < displays.count, displays[panel] != 0, let cursor = CGEvent(source: nil)?.location else { return }
        let target = CGDisplayBounds(displays[panel])
        if target.contains(cursor) { return }
        var rel = CGPoint(x: 0.5, y: 0.5)
        if let from = displays.filter({ $0 != 0 }).map(CGDisplayBounds).first(where: { $0.contains(cursor) }) {
            rel = CGPoint(x: (cursor.x - from.minX) / from.width, y: (cursor.y - from.minY) / from.height)
        }
        log("cursor: gaze moved to panel \(panel + 1) -> cursor follows from \(Int(cursor.x)),\(Int(cursor.y))")
        CGWarpMouseCursorPosition(CGPoint(x: target.minX + rel.x * target.width, y: target.minY + rel.y * target.height))
        CGAssociateMouseAndMouseCursorPosition(1)
    }

    // Full teardown, so the GUI can stop and start again in the same process.
    func stop() {
        guard !stopped else { return }
        stopped = true
        if isRecording { toggleRecording() }  // the file is finished, not lost
        depthLoading?.cancel()
        depthLoading = nil
        depth3D.route(panel: -1)
        depth3D.onFrame = nil
        if let activity { ProcessInfo.processInfo.endActivity(activity) }
        activity = nil
        HardwareCursor.show()
        statsTimer?.invalidate()
        gazeTimer?.invalidate()
        if let screenObserver { NotificationCenter.default.removeObserver(screenObserver) }
        displayLink?.invalidate()
        displayLink = nil
        renderThread?.cancel()
        renderThread = nil
        fallbackThread?.cancel()
        fallbackThread = nil
        hotKeys = nil  // unregisters the global hotkeys
        statusMenu = nil
        // Space at real size put the glasses in follow: back to pinned, how they are used without
        // Nimbo (the command goes out with the next IMU read, a few ms).
        if options.realSize && !options.glassesPinned && options.glassesMode == 0, let imu = pose as? ImuPoseSource {
            imu.setGlassesMode(1)
            Thread.sleep(forTimeInterval: 0.15)
        }
        pose?.stop()
        physicalDisplay = nil
        glassesID = nil
        let caps = captures + floatingCaptures
        captures = []
        floatingCaptures = []
        let done = DispatchSemaphore(value: 0)
        Task.detached {
            for c in caps { await c.stop() }
            done.signal()
        }
        _ = done.wait(timeout: .now() + 2)
        patternWindows.forEach { $0.close() }
        patternWindows = []
        window?.orderOut(nil)
        window = nil
        floatingDisplays = []
        floatingSerials = []
        desktop = nil  // virtual monitors disappear here
        // App-scoped display changes would otherwise last until the process exits.
        CGRestorePermanentDisplayConfiguration()
        isTracking = false
    }

    // MARK: input

    func adjustFov(_ delta: Float) {
        let ro = renderer.options
        ro.fovYDegrees = max(10, min(45, ro.fovYDegrees + delta))
        SettingsFile.shared.set(Double(ro.fovYDegrees), forKey: "fovY")
        options.fovYDeg = ro.fovYDegrees
        onFovChanged?(ro.fovYDegrees)
        requestRedraw()
        log(String(format: "fov: %.1f° (saved)", ro.fovYDegrees))
    }

    // Panel the head points at (left -> right), nil while calibrating or between panels.
    func gazePanel() -> Int? {
        guard let renderer, let p = pose?.latest(), p.tracking else { return nil }
        let i = renderer.panel(atGaze: p.orientation)
        return i >= 0 && i < panelDisplays.count ? i : nil
    }

    // ⌘⌥O focus mode: only the monitor (or floating window) you look at stays; ⌘⌥P cinema: that
    // one centred straight ahead and large. The same key again brings everything back; the
    // other key switches mode keeping the panel.
    private(set) var focusMode: Int = 0  // 0 all, 1 focus, 2 cinema

    func toggleFocus(cinema: Bool) {
        guard let renderer else { return }
        let ro = renderer.options
        let wanted = cinema ? 2 : 1
        if focusMode == wanted {
            focusMode = 0
            ro.focusPanel = -1
            ro.cinema = false
            log("focus: all panels")
            onNotice?("Todos los monitores")
        } else {
            if focusMode == 0 {
                // The panel you look at, else the cursor's; a floating window only when allowed
                // (else the first).
                let gaze = pose.latest().map { renderer.panel(atGaze: $0.orientation) } ?? -1
                let cursor = cursorPanel() ?? -1
                ro.focusPanel = [gaze, cursor].first { $0 >= 0 && eligible($0) } ?? 0
            }
            focusMode = wanted
            ro.cinema = cinema
            log("focus: \(cinema ? "cinema" : "focus") on panel \(ro.focusPanel + 1)")
            onNotice?(cinema ? "Modo cine (⌘⌥P para salir)" : "Modo enfoque (⌘⌥O para salir)")
        }
        updateDepth3D()
        requestRedraw()
    }

    // MARK: AI 3D

    // Captured frames: the cinema panel's go through the depth model first when AI 3D is on.
    // Capture queues.
    private func deliver(_ pb: CVPixelBuffer, panel: Int) {
        if depth3D.submit(pb, panel: panel) { return }
        renderer?.setSourcePixelBuffer(pb, forPanel: UInt(panel))
    }

    // The model finished loading (error nil) or could not be had. Main thread.
    private func depthModelReady(_ error: String?) {
        depthLoading = nil
        if let error {
            log("3d: model unavailable: \(error)")
            onNotice?("No se pudo preparar el 3D con IA")
            options.depth3D = false
            return
        }
        updateDepth3D()
    }

    // ⌘⌥T: AI 3D on / off for this session (⌘⌥1/2/3 are the app's layouts) (the app's setting is applied again when it changes).
    // On: cinema on the panel you look at, in depth (with the glasses in 3D). Off: flat, still
    // in cinema, to compare.
    func toggleDepth3D() {
        options.depth3D.toggle()
        log("3d: \(options.depth3D ? "on" : "off")")
        if options.depth3D {
            if renderer?.options.sideBySide == false {
                // Glasses in 2D: nothing to see in depth. Nothing changes; it says what to do.
                options.depth3D = false
                log("3d: glasses in 2D, not started")
                flash("Pon las gafas en modo 3D")
                flash("y pulsa ⌘⌥T")
                return
            }
            if focusMode != 2 {
                toggleFocus(cinema: true)
                cinemaFor3D = true
            }
        } else {
            onNotice?("3D con IA desactivado")
        }
        updateDepth3D()
    }

    // Gives depth to the cinema panel while AI 3D is on and each eye has its own image; else
    // every panel is flat. Loads the model the first time. Main thread.
    private func updateDepth3D() {
        guard let renderer, !stopped else { return }
        let ro = renderer.options
        let cinema = focusMode == 2 && ro.focusPanel >= 0
        let wanted = options.depth3D && cinema && ro.sideBySide ? ro.focusPanel : -1
        if options.depth3D && cinema && !ro.sideBySide {
            onNotice?("Pon las gafas en modo 3D para ver el cine en 3D")
            flash("Pon las gafas en modo 3D")
        }
        if wanted >= 0 && !depth3D.isLoaded {
            guard depthLoading == nil else { return }
            onNotice?("Preparando el 3D con IA…")
            flash("Preparando el 3D con IA…", seconds: 6)
            let engine = depth3D
            let owner = WeakServer(self)
            depthLoading = Task.detached(priority: .userInitiated) {
                let logger: @Sendable (String) -> Void = { line in DispatchQueue.main.async { owner.server?.log(line) } }
                do {
                    let url = try await DepthEngine.compiledModel(log: logger)
                    let t0 = Date()
                    try engine.load(url)
                    logger(String(format: "3d: model loaded in %.1f s (Neural Engine)", Date().timeIntervalSince(t0)))
                    DispatchQueue.main.async { owner.server?.depthModelReady(nil) }
                } catch {
                    let message = error.localizedDescription
                    DispatchQueue.main.async { owner.server?.depthModelReady(message) }
                }
            }
            return
        }
        guard wanted != depthPanel else { return }
        depth3D.route(panel: wanted)
        if depthPanel >= 0 { renderer.clearDepth(forPanel: UInt(depthPanel)) }
        if wanted >= 0 {
            log("3d: depth on panel \(wanted + 1) (strength \(ro.depthStrength), plane \(ro.depthConvergence))")
            onNotice?("Cine en 3D con IA (⌘⌥T para quitarlo)")
            flash("Cine en 3D con IA activado")
        } else if depthPanel >= 0 {
            log("3d: off")
            flash("Cine en 3D con IA desactivado")
        }
        depthPanel = wanted
        requestRedraw()
    }

    // ⌘⌥J: the cursor jumps to the centre of the monitor you are looking at (on demand, where
    // the gaze cursor would do it on its own).
    func jumpCursorToGaze() {
        guard let p = pose.latest(), p.tracking else { return }
        let i = renderer.panel(atGaze: p.orientation)
        if i >= monitorPanels, i - monitorPanels < floatingResolved.count {
            focusFloatingWindow(floatingResolved[i - monitorPanels])
            return
        }
        guard i >= 0, i < panelDisplays.count else {
            log("cursor: jump, no monitor in view")
            return
        }
        let b = CGDisplayBounds(panelDisplays[i])
        CGWarpMouseCursorPosition(CGPoint(x: b.midX, y: b.midY))
        CGAssociateMouseAndMouseCursorPosition(1)
        lastGazePanel = i
        log("cursor: jump to panel \(i + 1)")
    }

    func recenter() {
        // Glasses in their own pinned mode hold an anchor too. Setting pinned while in pinned does
        // nothing; a mode change recentres it (both axes): follow, then pinned again a moment later.
        if options.recenterGlasses || options.glassesPinned, let imu = pose as? ImuPoseSource {
            imu.setGlassesMode(0)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                imu.setGlassesMode(1)
                // Pinned: nothing of ours moves (the image is fitted), but the head's zero must
                // match the new anchor so the gaze finds the panel you look at.
                if self?.options.glassesPinned == true {
                    self?.pose.recenter()
                    self?.lastGazePanel = -2
                }
            }
            log("recenter: glasses' pinned anchor (follow -> pinned)")
        }
        if options.glassesPinned { return }
        pose.recenter()
        lastGazePanel = -2  // re-evaluate: the cursor follows to the panel in front
        log("recenter")
    }

    private func registerHotKeys() {
        let mods = cmdKey | optionKey
        guard let hotKeys else { return }
        // Logged: a shortcut another app holds never reaches Nimbo, and nothing else says so.
        let reg = { [weak self] (key: Int, name: String, action: @escaping () -> Void) in
            if !hotKeys.register(keyCode: key, modifiers: mods, { [weak self] in
                self?.log("hotkey: ⌘⌥\(name)")
                action()
            }) {
                self?.log("hotkey: ⌘⌥\(name) is taken by another app")
            }
        }
        reg(kVK_ANSI_C, "C") { [weak self] in self?.recenter() }
        reg(kVK_ANSI_Q, "Q") { [weak self] in self?.requestStop("stopped (⌘⌥Q)") }
        reg(kVK_ANSI_J, "J") { [weak self] in self?.jumpCursorToGaze() }
        reg(kVK_ANSI_O, "O") { [weak self] in self?.toggleFocus(cinema: false) }
        reg(kVK_ANSI_P, "P") { [weak self] in self?.toggleFocus(cinema: true) }
        reg(kVK_ANSI_R, "R") { [weak self] in self?.toggleRecording() }
        reg(kVK_ANSI_T, "T") { [weak self] in self?.toggleDepth3D() }
        // Field-of-view tuning with the glasses on: if the panels drift WITH the head when
        // turning, the render FOV is too large (Cmd+Opt+-); against it, too small (Cmd+Opt+=).
        reg(kVK_ANSI_Minus, "-") { [weak self] in self?.adjustFov(-0.5) }
        reg(kVK_ANSI_Equal, "=") { [weak self] in self?.adjustFov(0.5) }
        if let sim = pose as? SimPoseSource {
            let step: Float = 10
            hotKeys.register(keyCode: kVK_LeftArrow, modifiers: mods) { sim.nudge(yaw: step, pitch: 0) }
            hotKeys.register(keyCode: kVK_RightArrow, modifiers: mods) { sim.nudge(yaw: -step, pitch: 0) }
            hotKeys.register(keyCode: kVK_UpArrow, modifiers: mods) { sim.nudge(yaw: 0, pitch: step) }
            hotKeys.register(keyCode: kVK_DownArrow, modifiers: mods) { sim.nudge(yaw: 0, pitch: -step) }
        }
    }

    // MARK: capture

    @MainActor
    private func startCaptures(glasses: NSScreen?) async throws {
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
            let cap = ScreenCapture { [weak self] pb in self?.deliver(pb, panel: panel) }
            captures.append(cap)
            let renderWindow = CGWindowID(window.windowNumber)
            let (w, h) = try await cap.start(displayID: id, fps: fps, excludeOwnApp: exclude,
                                             excludeWindowIDs: [renderWindow], minScale: isVirtual && hiDPI ? 2 : 1)
            try Task.checkCancellation()
            log("capture: panel \(panel + 1) <- display \(id) \(w)x\(h) @\(fps) fps")
        }
    }

    // Gives macOS time to apply a display change; throws CancellationError if start was cancelled.
    private func settle(_ seconds: Double) async throws {
        try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
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

    // MARK: recording

    // ⌘⌥R: what the glasses show, recorded to Downloads. The render window is hidden from
    // captures (the monitors' captures exclude it by id anyway) except while recording.
    private var recorder: AnyObject?
    var isRecording: Bool { recorder != nil }

    func toggleRecording() {
        guard #available(macOS 15.0, *) else {
            onNotice?("Grabar vídeo necesita macOS 15 o posterior")
            return
        }
        guard let id = glassesID, let window else { return }
        if let r = recorder as? GlassesRecorder {
            recorder = nil
            Task { @MainActor in
                await r.stop()
                window.sharingType = .none
                self.log("record: saved \(r.url?.path ?? "")")
                self.onNotice?("Vídeo guardado en Descargas")
            }
            return
        }
        let r = GlassesRecorder()
        recorder = r
        window.sharingType = .readOnly
        Task { @MainActor in
            do {
                let url = try await r.start(displayID: id)
                self.log("record: started \(url.path)")
                self.onNotice?("Grabando lo que ves en las gafas (⌘⌥R para parar)")
            } catch {
                self.recorder = nil
                window.sharingType = .none
                self.log("record: \(error.localizedDescription)")
                self.onNotice?("No se pudo grabar: \(error.localizedDescription)")
            }
        }
    }

    // The display link stops for good when macOS moves the glasses display to another Space
    // (a window dragged to another desktop): the glasses froze. Bring the window back on the
    // glasses and start a fresh link.
    private var stalledSeconds = 0

    private func recoverRenderLoop() {
        stalledSeconds = 0
        guard let window, let view else { return }
        startFallbackLoop()
        log("render: stalled (the glasses display changed Space?), restarting the render loop")
        if let id = glassesID, let s = NSScreen.screens.first(where: { Displays.id(of: $0) == id }) {
            window.setFrame(s.frame, display: true)
        }
        window.orderFrontRegardless()
        displayLink?.invalidate()
        renderThread?.cancel()
        startRenderLoop(layer: view.metalLayer)
        requestRedraw()
    }

    private func startRenderLoop(layer: CAMetalLayer) {
        let link = CAMetalDisplayLink(metalLayer: layer)
        link.delegate = self
        // Two frames: with one, frames reached the glasses 26 or 35 ms after the sample, in
        // streaks, and the pose predicted for one moment was shown at the other (the panels
        // jittered while the head moved). Steady 35 ms predicts right every frame: simulated,
        // 0.18 -> 0.15 deg rms moving, peaks 0.49 -> 0.34.
        link.preferredFrameLatency = 2
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
        statsLock.lock()
        linkFrames &+= 1
        let margin = latchMarginS
        statsLock.unlock()
        if options.lateLatch {
            // targetTimestamp: when the frame must be done. Sleep until just before it, then
            // read the freshest pose (the IMU thread keeps it at ~457 Hz).
            let wait = update.targetTimestamp - margin - CACurrentMediaTime()
            if wait > 0.0003 && wait < 0.03 { Thread.sleep(forTimeInterval: wait) }
        }
        drawFrame(update.drawable, targetMedia: update.targetPresentationTimestamp)
    }

    // One frame into `drawable`, for presentation at `targetMedia` (CACurrentMediaTime clock).
    // From the display link, or from the fallback loop while it is stalled. false: skipped
    // (nothing changed), not presented.
    @discardableResult
    private func drawFrame(_ drawable: CAMetalDrawable, targetMedia: CFTimeInterval) -> Bool {
        drawLock.lock()
        defer { drawLock.unlock() }
        // Map CACurrentMediaTime (display link base) onto the engine's steady clock.
        let nowMedia = CACurrentMediaTime()
        let nowHost = RNSpatialEngine.hostNowNs()
        statsLock.lock()
        let late = max(0, min(25, presentLateMs))
        statsLock.unlock()
        let targetHost = Int64(nowHost) + Int64((targetMedia - nowMedia + late / 1e3) * 1e9)

        guard let p = pose.latest() else {
            renderer.invalidateSkippedFrame()
            _ = renderer.render(to: drawable, orientation: simd_quatf(ix: 0, iy: 0, iz: 0, r: 1),
                                angularVelocity: .zero, sampleHostNs: 0, targetHostNs: 0)
            return true
        }
        if nothingChanged(p) {
            statsLock.lock()
            stats.idle += 1
            stats.tracking = p.tracking
            statsLock.unlock()
            isTracking = p.tracking
            return false
        }
        // Ground truth: when the frame actually reached the display (same media clock).
        let sampleMedia = nowMedia - Double(Int64(nowHost) - Int64(p.hostTimeNs)) / 1e9
        let target = targetMedia
        drawable.addPresentedHandler { [weak self] d in
            guard let self, d.presentedTime > 0 else { return }
            self.statsLock.lock()
            let lateMs = (d.presentedTime - target) * 1e3
            self.stats.addPresented(latency: (d.presentedTime - sampleMedia) * 1e3, targetError: lateMs)
            if lateMs > max(3, self.framePeriodS * 500) {  // missed its frame: read earlier
                self.latchMarginS = min(0.012, self.latchMarginS + 0.001)
                self.latchOnTime = 0
            } else {
                self.latchOnTime += 1
                if self.latchOnTime >= 240 {
                    self.latchMarginS = max(0.002, self.latchMarginS - 0.00025)
                    self.latchOnTime = 0
                }
            }
            self.presentLateMs += (max(-10, min(40, lateMs)) - self.presentLateMs) * 0.03
            let gap = d.presentedTime - self.lastPresentedTime
            if self.lastPresentedTime > 0, gap > 0, gap < 0.05 { self.stats.addInterval(gap * 1e3) }
            self.lastPresentedTime = d.presentedTime
            self.statsLock.unlock()
        }
        let prediction = renderer.render(to: drawable, orientation: p.orientation,
                                         angularVelocity: p.angularVelocity,
                                         sampleHostNs: p.hostTimeNs, targetHostNs: UInt64(max(0, targetHost)))
        let poseAge = Double(Int64(nowHost) - Int64(p.hostTimeNs)) / 1e6
        let photon = Double(targetHost - Int64(p.hostTimeNs)) / 1e6
        let still = simd_length(p.angularVelocity) < 0.0175
        // From the relative rotation's vector part: acos of a dot product near 1 rounds every
        // step under ~0.02 deg to 0 in Float (the log read 0 while the panels trembled).
        var step = 0.0
        if still, let last = lastDrawn {
            let r = simd_mul(simd_inverse(last), p.orientation)
            step = 2 * asin(min(1, Double(simd_length(r.imag)))) * 180 / .pi
        }
        lastDrawn = p.orientation
        statsLock.lock()
        stats.add(poseAge: poseAge, photon: photon, prediction: Double(prediction))
        stats.tracking = p.tracking
        if still && step < 1 {
            stats.stillN += 1
            stats.stillSq += step * step
        }
        statsLock.unlock()
        isTracking = p.tracking
        return true
    }

    // MARK: fallback render loop

    // Mission Control (three fingers up) stops the display link for good: the glasses froze,
    // and new links did not start either. While it is stalled, frames are drawn here, paced by
    // the layer's drawables (vsync); it ends as soon as the display link ticks again.
    private func startFallbackLoop() {
        guard fallbackThread == nil, let view, let window else { return }
        let layer = view.metalLayer
        statsLock.lock()
        let since = linkFrames
        statsLock.unlock()
        let hz = max(60, glassesID.flatMap { CGDisplayCopyDisplayMode($0)?.refreshRate } ?? 60)
        log("render: display link stalled (window " + (window.occlusionState.contains(.visible) ? "visible" : "not visible")
            + "); drawing on our own clock until it is back")
        let thread = Thread { [weak self] in
            var frames = 0
            while !Thread.current.isCancelled {
                guard let self else { return }
                self.statsLock.lock()
                let ticked = self.linkFrames != since
                self.statsLock.unlock()
                if ticked { break }
                var drew = false
                autoreleasepool {
                    if let d = layer.nextDrawable() {  // waits for a free one: vsync pacing (1 s at most)
                        drew = self.drawFrame(d, targetMedia: CACurrentMediaTime() + 1.5 / hz)
                    }
                }
                if drew { frames += 1 } else { Thread.sleep(forTimeInterval: 1 / hz) }
            }
            DispatchQueue.main.async { [weak self] in
                self?.fallbackThread = nil
                self?.log("render: display link back (\(frames) frames drawn on our own clock)")
            }
        }
        thread.name = "rayneo.render.fallback"
        thread.qualityOfService = .userInteractive
        thread.start()
        fallbackThread = thread
    }

    private func printStats() {
        statsLock.lock()
        let s = stats
        stats = LatencyStats()
        statsLock.unlock()
        let capFrames = (captures + floatingCaptures).reduce(UInt64(0)) { $0 &+ $1.frames }
        let capFps = capFrames &- lastCaptureFrames
        lastCaptureFrames = capFrames
        guard s.frames + s.idle > 0 else {
            log("render: no frames")
            stalledSeconds += 1
            if stalledSeconds >= 2 { recoverRenderLoop() }
            return
        }
        stalledSeconds = 0
        if overlayShown && s.tracking {
            renderer.setOverlayText(nil)
            overlayShown = false
            log("ready: tracking")
            if !pendingFlash.isEmpty {
                let lines = pendingFlash
                pendingFlash = []
                lines.forEach { flash($0) }
            }
        } else if overlayShown {
            let left = max(1, 6 - calibSeconds)
            renderer.setOverlayText("Nimbo se está preparando\nDeja las gafas quietas · \(left) s")
        }
        if !s.tracking && pose is ImuPoseSource {
            calibSeconds += 1
            if calibSeconds % 3 == 1 {
                log("CALIB: head tracking starts after ~6 s with the glasses still on the table" +
                    " (or ~14 s if you are already wearing them)")
            }
        }
        statsLock.lock()
        let latchMs = options.lateLatch ? latchMarginS * 1e3 : 0
        statsLock.unlock()
        let n = Double(max(1, s.frames))
        let pn = Double(max(1, s.presentedN))
        let inN = Double(max(1, s.intervalN))
        let inMean = s.intervalSum / inN
        let inStd = (max(0, s.intervalSq / inN - inMean * inMean)).squareRoot()
        log(String(format: "%@ render %3d fps%@ | capture %3llu fps (%d src) | pose age %.1f/%.1f ms | sample->presented %.1f/%.1f ms | predicted %.1f ms (late %.1f) | frame interval %.1f±%.1f max %.1f ms | latch %.1f ms%@",
                   s.tracking ? "TRACK" : "CALIB", s.frames, s.idle > 0 ? " (\(s.idle) idle)" as NSString : "" as NSString, capFps, captures.count, s.poseAgeSum / n, s.poseAgeMax,
                   s.presentedSum / pn, s.presentedMax, s.predSum / n, s.targetErrSum / pn, inMean, inStd, s.intervalMax, latchMs,
                   s.stillN > 30 ? String(format: " | still wobble %.4f°/frame", (s.stillSq / Double(s.stillN)).squareRoot()) as NSString : "" as NSString))
        if depthPanel >= 0 {
            let d = depth3D.takeStats()
            let n = Double(max(1, d.frames))
            log(String(format: "3d: %d depth fps (%d on GPU, %d dropped, %d cut(s)) | scale %.1f ms | infer %.1f ms | in order %.1f ms",
                       d.frames, d.gpuFrames, d.dropped, d.cuts, d.scaleMs / n, d.inferMs / n, d.waitMs / n))
        }
    }

    // MARK: util

    private func fail(_ msg: String) -> NSError {
        NSError(domain: "SpatialServer", code: 1, userInfo: [NSLocalizedDescriptionKey: msg])
    }

    // stdout (CLI) and ~/Library/Application Support/Nimbo/nimbo.log (the app has no console).
    func log(_ s: String) {
        print(s)
        fflush(stdout)
        Self.appendToFile(s)
    }

    private static let logURL: URL = {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Nimbo", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("nimbo.log")
    }()

    private static let logFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    private static func appendToFile(_ s: String) {
        let line = "\(logFormatter.string(from: Date())) \(s)\n"
        guard let data = line.data(using: .utf8) else { return }
        if let attrs = try? FileManager.default.attributesOfItem(atPath: logURL.path),
           (attrs[.size] as? Int ?? 0) > 5_000_000 {
            try? FileManager.default.removeItem(at: logURL)  // keep it small
        }
        if let h = try? FileHandle(forWritingTo: logURL) {
            h.seekToEndOfFile()
            h.write(data)
            try? h.close()
        } else {
            try? data.write(to: logURL)
        }
    }

    // Diagnostics for cursor problems: every warp with its reason, and changes of the front
    // app and of the hardware cursor's visibility.
    private var lastCursorState = ""

    private func logCursorState() {
        let front = NSWorkspace.shared.frontmostApplication?.localizedName ?? "?"
        let state = "front app \(front), hardware cursor \(HardwareCursor.isHidden ? "hidden by Nimbo" : "shown")"
        if state != lastCursorState {
            lastCursorState = state
            log("cursor: " + state)
        }
    }
}

// The server seen from a background task without keeping it alive (it is used on the main
// thread only, where the task hands its result back).
private final class WeakServer: @unchecked Sendable {
    weak var server: SpatialServer?
    init(_ server: SpatialServer) { self.server = server }
}
