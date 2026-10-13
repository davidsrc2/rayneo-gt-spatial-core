import Foundation

struct ServerOptions {
    var sideBySide = true
    var ipdMM: Float = 63
    var distanceM: Float = 2.0
    var arcDeg: Float = 50
    var fovYDeg: Float = 24.5
    var k1: Float = 0
    var k2: Float = 0
    var source = "auto"           // auto | glasses | builtin | <CGDirectDisplayID>
    var calibrationPath = "calibration.json"
    var calibrationRequired = false
    var refresh120 = true
    var unmirror = false
    var windowed = false          // debug: render in a window on the main screen
    var durationS: Double = 0     // 0 = until Ctrl+C

    static let usage = """
    usage: mac_spatial_server [options]
      --mono               single full view (glasses in 2D mode); default half-SBS (3D mode)
      --ipd MM             interpupillary distance (63)
      --distance M         virtual screen radius in metres (2.0)
      --arc DEG            horizontal extent of the curved screen (50)
      --fov DEG            per-eye vertical FOV of the optics (24.5)
      --k1 K --k2 K        barrel pre-distortion (0 = off)
      --source S           capture: auto | glasses | builtin | <display id>
      --calib FILE         sensor_to_head calibration (default ./calibration.json if present)
      --unmirror           extend instead of mirror while running and capture the built-in
                           display (reverted on exit; measured +17 ms latency vs mirrored)
      --60hz               keep the current refresh rate (default: switch glasses to 120 Hz)
      --windowed           debug window on the main screen instead of the glasses
      --duration S         exit after S seconds
    hotkeys: Cmd+Opt+C recenter, Cmd+Opt+Q quit
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
            case "--mono": o.sideBySide = false
            case "--ipd": guard let v = float() else { return nil }; o.ipdMM = v
            case "--distance": guard let v = float() else { return nil }; o.distanceM = v
            case "--arc": guard let v = float() else { return nil }; o.arcDeg = v
            case "--fov": guard let v = float() else { return nil }; o.fovYDeg = v
            case "--k1": guard let v = float() else { return nil }; o.k1 = v
            case "--k2": guard let v = float() else { return nil }; o.k2 = v
            case "--source": guard let v = value() else { return nil }; o.source = v
            case "--calib":
                guard let v = value() else { return nil }
                o.calibrationPath = v
                o.calibrationRequired = true
            case "--60hz": o.refresh120 = false
            case "--unmirror": o.unmirror = true

            case "--windowed": o.windowed = true
            case "--duration": guard let v = value().flatMap({ Double($0) }) else { return nil }; o.durationS = v
            default: return nil
            }
            i += 1
        }
        return o
    }
}
