import Foundation

struct ServerOptions {
    var sideBySide: Bool? = nil   // nil: SBS on the glasses, mono in --windowed
    var ipdMM: Float = 63
    var distanceM: Float = 2.0
    var arcDeg: Float? = nil      // nil: 38 per virtual panel, 50 for single-display capture
    var gapDeg: Float = 1
    var curved = false
    var fovYDeg: Float = 24.5
    var k1: Float = 0
    var k2: Float = 0

    var displays = 2              // virtual monitors; 0 = capture an existing display
    var vdWidth = 1152            // points; with hiDPI: large, sharp text
    var vdHeight = 648
    var hiDPI = true
    var testPattern = false
    var fit = false               // size panels so all of them are visible at once
    var includeMain = false       // --keep-main: glasses display stays main, shown as a panel
    var gatherWindows = true      // move windows off the glasses display (Accessibility)
    var gazeCursor = true         // cursor jumps to the panel you look at

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
    usage: mac_spatial_server [options]
    layout
      --displays N         virtual monitors in a row around you (2); 0 = capture an existing display
      --vd-size WxH        virtual monitor size in points (1152x648: large text)
      --no-hidpi           1x virtual monitors (less sharp, 4x cheaper to capture)
      --fit                all panels visible at once (each at a fraction of full resolution)
      --arc DEG            horizontal size of each panel (38; 50 with --displays 0)
      --gap DEG            space between panels (1)
      --curved             curved panels (default: flat monitors angled towards you)
      --distance M         panel radius in metres (2.0)
      --test-pattern       labelled grid on each virtual monitor
      --keep-main          keep the glasses display as macOS's main display, shown as the left
                           panel (two cursors: macOS draws one on the glasses surface)
      --no-gather          do not move windows from the glasses display to panel 1
      --no-gaze-cursor     keep the cursor where it is when you look at another panel
    optics
      --mono / --sbs       single view (glasses 2D) / half side-by-side (glasses 3D)
      --ipd MM             interpupillary distance (63)
      --fov DEG            per-eye vertical FOV of the optics (24.5)
      --k1 K --k2 K        barrel pre-distortion (0 = off)
    tracking
      --calib FILE         sensor_to_head calibration (default ./calibration.json if present)
      --sim                no glasses: simulated head (Cmd+Opt+arrows to look around)
      --sim-sweep DEG      sim: automatic left/right sweep of +-DEG
    display
      --windowed           render in a window on the Mac screen instead of the glasses
      --source S           --displays 0 only: auto | glasses | builtin | <display id>
      --unmirror           extend instead of mirror while running (reverted on exit)
      --60hz               keep the glasses' current refresh rate (default: 120 Hz)
      --duration S         exit after S seconds
    menu bar: "◎ RayNeo" -> Recentrar / Salir
    hotkeys: Cmd+Opt+C recenter (and bring the cursor to the panel in front), Cmd+Opt+Q quit,
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
            case "--vd-size":
                let parts = value()?.split(separator: "x").compactMap { Int($0) } ?? []
                guard parts.count == 2, parts[0] >= 640, parts[1] >= 480 else { return nil }
                o.vdWidth = parts[0]
                o.vdHeight = parts[1]
            case "--hidpi": o.hiDPI = true
            case "--no-hidpi": o.hiDPI = false
            case "--arc": guard let v = float() else { return nil }; o.arcDeg = v
            case "--gap": guard let v = float() else { return nil }; o.gapDeg = v
            case "--curved": o.curved = true
            case "--distance": guard let v = float() else { return nil }; o.distanceM = v
            case "--test-pattern": o.testPattern = true
            case "--keep-main": o.includeMain = true
            case "--no-main": o.includeMain = false
            case "--no-gather": o.gatherWindows = false
            case "--fit": o.fit = true
            case "--no-gaze-cursor": o.gazeCursor = false
            case "--mono": o.sideBySide = false
            case "--sbs": o.sideBySide = true
            case "--ipd": guard let v = float() else { return nil }; o.ipdMM = v
            case "--fov": guard let v = float() else { return nil }; o.fovYDeg = v
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
