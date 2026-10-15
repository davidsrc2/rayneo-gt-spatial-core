import Foundation

// Cell of a monitor in the layout grid (row 0 = top).
struct GridPos: Codable, Hashable {
    var col: Int
    var row: Int
}

// A monitor placed by hand (layout editor): degrees right / up at the 2 m reference with the
// common panel size, and its own size (core::PanelPose).
struct PanelPos: Codable, Hashable {
    var yaw: Float
    var pitch: Float
    var scale: Float = 1
}

// A single window shown as a panel of its own (a video above the monitors...). Identified by
// its window id, or by text in its title / app name (CLI). pose nil: centred above the monitors.
struct FloatingWindow: Codable, Hashable {
    var windowID: UInt32 = 0
    var match = ""
    var title = ""          // shown in the app: "App — window title"
    var pose: PanelPos? = nil
    var app = ""            // remembered between sessions: found again by app and title
    var windowTitle = ""
}

struct ServerOptions {
    var sideBySide: Bool? = nil   // nil: SBS on the glasses, mono in --windowed
    var ipdMM: Float = 63
    var distanceM: Float = 2.0
    var arcDeg: Float? = nil      // nil: 38 per virtual panel, 50 for single-display capture
    var gapDeg: Float = 1
    var curved = false
    var curvature: Float = 0          // 0 flat .. 1 cylinder (--curved = 1)
    var tilt: Float = 1               // panels above / below: 1 face you .. 0 upright
    var fovYDeg: Float = 24.5
    var fovFromCLI = false
    var deadZoneDeg: Float = 1.0
    var headless = true           // CLI: own menu bar item, Cmd+Opt+Q quits the process
    var k1: Float = 0
    var k2: Float = 0

    var displays = 2              // virtual monitors; 0 = capture an existing display
    var layout: [GridPos]? = nil  // grid cell per monitor; nil = one row
    var panelPoses: [PanelPos]? = nil  // hand-placed monitors (same count as layout); nil = the grid
    var floatingWindows: [FloatingWindow] = []  // single windows as panels, after the monitors
    var startFocusPanel = 0       // start in focus mode on this panel (1-based); 0 = all panels
    var startCinema = false       // with startFocusPanel: cinema mode
    var vdWidth = 1152            // points; with hiDPI: large, sharp text
    var vdHeight = 648
    var hiDPI = true
    var testPattern = false
    var fit = false               // size panels so all of them are visible at once
    var includeMain = false       // --keep-main: glasses display stays main, shown as a panel
    var macScreen = false         // --mac-screen: the Mac's own screen is panel 1 (lid open), the rest virtual
    var gatherWindows = true      // move windows off the glasses display (Accessibility)
    var gazeCursor = true         // cursor jumps to the panel you look at
    var dimOthers: Float = 1      // brightness of the panels you are not looking at (1 = off)
    var trackingAxes = 1          // head rotations that move the panels: 0 both, 1 left/right, 2 up/down
    var glassesPinned = false     // glasses in their own pinned mode: panels fixed in the image, fitted inside it
    var realSize = false          // not pinned: each panel the size of the glasses' own image, the space moves with the head
    var growFocused = true        // pinned: the panel you look at (1 s) or take the mouse to fills the image
    var focusWindows = true       // floating windows can be that panel (and focus / cinema's) too
    var glassesMode = -1          // set the glasses' mode at start: 0 follow, 1 pinned, 2 steady; -1 = leave
    var brightness = 0            // set the glasses' brightness 1..8 at start; 0 = leave
    var recenterGlasses = false   // ⌘⌥C also recentres the glasses' own pinned mode (0x08 pinned)

    var source = "auto"           // --displays 0 only: auto | glasses | builtin | <id>
    var calibrationPath = "calibration.json"
    var calibrationRequired = false
    var refresh120 = true
    var unmirror = false
    var windowed = false          // render in a window on the main screen
    var sim = false               // simulated head instead of the IMU
    var simSweepDeg: Float = 0    // automatic yaw sweep amplitude (sim)
    var durationS: Double = 0     // 0 = until Ctrl+C

    var panelArcDeg: Float { arcDeg ?? (displays > 0 ? 38 : 50) }  // 38: a whole panel fits the ~42° horizontal FOV

    static let usage = """
    usage: Nimbo [options]
    layout
      --displays N         virtual monitors in a row around you (2); 0 = capture an existing display
      --layout C,R;C,R...  grid cell per monitor (row 0 = top), e.g. "0,0;0,1" = two stacked
      --vd-size WxH        virtual monitor size in points (1152x648: large text)
      --no-hidpi           1x virtual monitors (less sharp, 4x cheaper to capture)
      --fit                all panels visible at once (each at a fraction of full resolution)
      --arc DEG            horizontal size of each panel (38; 50 with --displays 0)
      --gap DEG            space between panels (1)
      --curved             curved panels (default: flat monitors angled towards you)
      --curvature K        0 flat .. 1 fully curved around you
      --distance M         panel radius in metres (2.0)
      --poses Y,P,S;...    hand-placed monitors: degrees right, degrees up and size (1 = normal)
      --window TEXT[@Y,P,S] float the window whose title or app contains TEXT as a panel of its
                           own (default: centred above the monitors); repeatable
      --focus-panel N      start showing only panel N (focus mode, Cmd+Opt+O)
      --cinema             with --focus-panel: that panel centred and large (Cmd+Opt+P)
      --test-pattern       labelled grid on each virtual monitor
      --mac-screen         the Mac's own screen is panel 1 (when the lid is open); the other
                           panels are virtual monitors next to it
      --keep-main          keep the glasses display as macOS's main display, shown as the left
                           panel (two cursors: macOS draws one on the glasses surface)
      --no-gather          do not move windows from the glasses display to panel 1
      --no-gaze-cursor     keep the cursor where it is when you look at another panel
      --dim B              brightness of the panels you are not looking at, 0..1 (1 = off)
      --pinned             the glasses are in their pinned mode: they anchor the image, so the
                           panels stay put in it and are sized to fit inside it (sets that mode)
      --real-size          glasses in follow: every panel as large as the glasses' own image, fixed in
                           the room on both axes (turn to see the next one)
      --no-grow            pinned: always the whole arrangement (else the panel you look at fills the image)
      --focus-monitors     only monitors fill the image / are focused, never floating windows
      --glasses-mode M     put the glasses in follow, steady or pinned (pinned implies --pinned)
      --brightness N       glasses' brightness 1..8 (8 = factory level)
      --recenter-glasses   Cmd+Opt+C also recentres the glasses' own pinned mode
      --track AXES         head rotations that move the panels: h (left/right, default: still
                           vertically), v (up/down, for stacked monitors) or both
    optics
      --mono / --sbs       single view (glasses 2D) / half side-by-side (glasses 3D)
      --ipd MM             interpupillary distance (63)
      --fov DEG            per-eye vertical FOV of the optics (24.5, or the value tuned with
                           Cmd+Opt+- / Cmd+Opt+= which is remembered)
      --k1 K --k2 K        barrel pre-distortion (0 = off)
    tracking
      --calib FILE         sensor_to_head calibration (default ./calibration.json if present)
      --dead-zone DEG      stabilizer dead zone (1.0): smaller follows sooner, larger is stiller
      --sim                no glasses: simulated head (Cmd+Opt+arrows to look around)
      --sim-sweep DEG      sim: automatic left/right sweep of +-DEG
    display
      --windowed           render in a window on the Mac screen instead of the glasses
      --source S           --displays 0 only: auto | glasses | builtin | <display id>
      --unmirror           extend instead of mirror while running (reverted on exit)
      --60hz               keep the glasses' current refresh rate (default: 120 Hz)
      --duration S         exit after S seconds
    menu bar: "◎ Nimbo" -> Recentrar / Salir
    hotkeys: Cmd+Opt+C recenter (and bring the cursor to the panel in front), Cmd+Opt+Q quit,
             Cmd+Opt+- / Cmd+Opt+= field of view (tune until the panels stay put when turning),
             Cmd+Opt+arrows look around (--sim)
    """

    static func parse(_ args: [String]) -> ServerOptions? {
        var o = ServerOptions()
        var i = 1
        func value() -> String? {
            i += 1
            return i < args.count ? args[i] : nil
        }
        func float() -> Float? { value().flatMap { Float($0) } }
        while i < args.count {
            switch args[i] {
            case "--displays": guard let v = value().flatMap({ Int($0) }), (0...8).contains(v) else { return nil }; o.displays = v
            case "--layout":
                let cells = value()?.split(separator: ";").map { $0.split(separator: ",").compactMap { Int($0) } } ?? []
                guard !cells.isEmpty, cells.count <= 8, cells.allSatisfy({ $0.count == 2 }) else { return nil }
                o.layout = cells.map { GridPos(col: $0[0], row: $0[1]) }
                o.displays = cells.count
            case "--poses":
                let poses = value()?.split(separator: ";").map { $0.split(separator: ",").compactMap { Float($0) } } ?? []
                guard !poses.isEmpty, poses.count <= 8, poses.allSatisfy({ $0.count == 3 }) else { return nil }
                o.panelPoses = poses.map { PanelPos(yaw: $0[0], pitch: $0[1], scale: $0[2]) }
            case "--window":
                guard let v = value(), !v.isEmpty else { return nil }
                let parts = v.split(separator: "@", maxSplits: 1).map(String.init)
                var w = FloatingWindow(match: parts[0])
                if parts.count == 2 {
                    let n = parts[1].split(separator: ",").compactMap { Float($0) }
                    guard n.count == 3 else { return nil }
                    w.pose = PanelPos(yaw: n[0], pitch: n[1], scale: n[2])
                }
                o.floatingWindows.append(w)
            case "--focus-panel": guard let v = value().flatMap({ Int($0) }), (1...8).contains(v) else { return nil }; o.startFocusPanel = v
            case "--cinema": o.startCinema = true
            case "--dead-zone": guard let v = float() else { return nil }; o.deadZoneDeg = v
            case "--vd-size":
                let parts = value()?.split(separator: "x").compactMap { Int($0) } ?? []
                guard parts.count == 2, parts[0] >= 640, parts[1] >= 480 else { return nil }
                o.vdWidth = parts[0]
                o.vdHeight = parts[1]
            case "--hidpi": o.hiDPI = true
            case "--no-hidpi": o.hiDPI = false
            case "--arc": guard let v = float() else { return nil }; o.arcDeg = v
            case "--gap": guard let v = float() else { return nil }; o.gapDeg = v
            case "--curved": o.curved = true; o.curvature = 1
            case "--curvature": guard let v = float(), (0...1).contains(v) else { return nil }; o.curvature = v
            case "--tilt": guard let v = float(), (0...1).contains(v) else { return nil }; o.tilt = v
            case "--distance": guard let v = float() else { return nil }; o.distanceM = v
            case "--test-pattern": o.testPattern = true
            case "--keep-main": o.includeMain = true
            case "--mac-screen": o.macScreen = true
            case "--no-main": o.includeMain = false
            case "--no-gather": o.gatherWindows = false
            case "--fit": o.fit = true
            case "--no-gaze-cursor": o.gazeCursor = false
            case "--track":
                guard let v = value(), let axes = ["both": 0, "h": 1, "v": 2][v] else { return nil }
                o.trackingAxes = axes
            case "--full-tracking": o.trackingAxes = 0
            case "--pinned": o.glassesPinned = true; o.glassesMode = 1
            case "--no-grow": o.growFocused = false
            case "--real-size": o.realSize = true
            case "--focus-monitors": o.focusWindows = false
            case "--glasses-mode":
                guard let v = value(), let m = ["follow": 0, "pinned": 1, "steady": 2][v] else { return nil }
                o.glassesMode = m
                o.glassesPinned = m == 1
            case "--recenter-glasses": o.recenterGlasses = true
            case "--brightness": guard let v = value().flatMap({ Int($0) }), (1...8).contains(v) else { return nil }; o.brightness = v
            case "--dim": guard let v = float() else { return nil }; o.dimOthers = max(0, min(1, v))
            case "--mono": o.sideBySide = false
            case "--sbs": o.sideBySide = true
            case "--ipd": guard let v = float() else { return nil }; o.ipdMM = v
            case "--fov": guard let v = float() else { return nil }; o.fovYDeg = v; o.fovFromCLI = true
            case "--k1": guard let v = float() else { return nil }; o.k1 = v
            case "--k2": guard let v = float() else { return nil }; o.k2 = v
            case "--calib":
                guard let v = value() else { return nil }
                o.calibrationPath = v
                o.calibrationRequired = true
            case "--sim": o.sim = true
            case "--sim-sweep": guard let v = float() else { return nil }; o.sim = true; o.simSweepDeg = v
            case "--windowed": o.windowed = true
            case "--source": guard let v = value() else { return nil }; o.source = v
            case "--unmirror": o.unmirror = true
            case "--60hz": o.refresh120 = false
            case "--duration": guard let v = value().flatMap({ Double($0) }) else { return nil }; o.durationS = v
            default: return nil
            }
            i += 1
        }
        return o
    }
}
