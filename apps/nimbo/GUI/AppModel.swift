import AppKit
import Combine
import Foundation

// Every user-facing setting, persisted in settings.json (SettingsFile, the same file as on
// Windows), and how it maps to ServerOptions.
final class AppModel: ObservableObject {
    struct TextSize {
        let width: Int
        let height: Int
        let label: String
    }

    // Left = small text (more space), right = large text.
    static let textSizes = [
        TextSize(width: 1920, height: 1080, label: "Normal"),
        TextSize(width: 1600, height: 900, label: "Algo más grande"),
        TextSize(width: 1366, height: 768, label: "Grande"),
        TextSize(width: 1152, height: 648, label: "Más grande"),
        TextSize(width: 1024, height: 576, label: "Muy grande"),
    ]

    private let d = SettingsFile.shared

    @Published var cells: [GridPos] {
        didSet {
            save(cells, "layoutCells")
            // A new arrangement from the grid editor or a template starts from the grid again.
            if !keepingPoses && !panelPoses.isEmpty { panelPoses = [] }
        }
    }
    // Hand-placed monitors (layout editor); empty = the grid. Same count and order as cells.
    @Published var panelPoses: [PanelPos] { didSet { save(panelPoses, "panels") } }
    // Single windows floating as panels, remembered by app and title (window ids do not survive
    // the app: found again when Nimbo starts).
    @Published var floating: [FloatingWindow] { didSet { save(floating, "floatingWindows") } }

    private var keepingPoses = false
    @Published var textSize: Int { didSet { d.set(textSize, forKey: "textSize") } }
    @Published var distance: Double { didSet { d.set(distance, forKey: "distance") } }
    @Published var gap: Double { didSet { d.set(gap, forKey: "gap") } }
    @Published var curvature: Double { didSet { d.set(curvature, forKey: "curvature") } }  // 0 flat .. 1 cylinder
    @Published var tilt: Double { didSet { d.set(tilt, forKey: "tilt") } }  // panels above / below: 1 face you .. 0 upright
    @Published var size: Double { didSet { d.set(size, forKey: "size") } }                // panel width, degrees
    @Published var autoStart: Bool { didSet { d.set(autoStart, forKey: "autoStart") } }
    var curved: Bool {
        get { curvature > 0.5 }
        set { curvature = newValue ? 1 : 0 }
    }

    // Advanced
    @Published var fov: Double { didSet { d.set(fov, forKey: "fovY") } }  // the glasses' pinned mode
    // The space at real size: the optics' vertical field of view (NormalMode.opticalVFovDeg).
    @Published var opticalVFov: Double { didSet { d.set(opticalVFov, forKey: "opticalVFov") } }
    @Published var deadZone: Double { didSet { d.set(deadZone, forKey: "deadZone") } }
    @Published var gazeCursor: Bool { didSet { d.set(gazeCursor, forKey: "gazeCursor") } }
    @Published var dimOthers: Bool { didSet { d.set(dimOthers, forKey: "dimOthers") } }
    // Around the panels (⌘⌥F cycles): none, Nimbo's own (rain, stars, sunset, aurora) or a picture.
    static let backgrounds = ["Ninguno (se ve la habitación)", "Lluvia", "Estrellas", "Atardecer", "Aurora", "Imagen propia"]
    @Published var background: Int { didSet { d.set(background, forKey: "background") } }
    @Published var backgroundImage: String { didSet { d.set(backgroundImage, forKey: "backgroundImage") } }
    @Published var backgroundBrightness: Double { didSet { d.set(backgroundBrightness, forKey: "backgroundBrightness") } }
    @Published var gatherWindows: Bool { didSet { d.set(gatherWindows, forKey: "gatherWindows") } }
    @Published var tracking: Int { didSet { d.set(tracking, forKey: "tracking") } }  // 0 both, 1 left/right, 2 up/down
    // How the panels are seen: true = one panel large in the glasses' pinned image, false = the
    // space at real size with the glasses in follow. Nimbo sets the glasses' mode to match.
    @Published var glassesPinned: Bool { didSet { d.set(glassesPinned ? 1 : 0, forKey: "glassesMode") } }
    @Published var anchorAtRest: Bool { didSet { d.set(anchorAtRest, forKey: "anchorAtRest") } }  // panels frozen while the head is still
    @Published var vehicleMode: Bool { didSet { d.set(vehicleMode, forKey: "vehicleMode") } }  // a quarter of turns followed (in a car)
    @Published var flatWall: Bool { didSet { d.set(flatWall, forKey: "flatWall") } }  // one plane, edge to edge
    @Published var keyHighlight: Bool { didSet { d.set(keyHighlight, forKey: "keyHighlight") } }  // frame the keyboard's panel
    @Published var macScreen: Bool { didSet { d.set(macScreen, forKey: "macScreen") } }  // the Mac's screen is panel 1
    // With the Mac's screen as panel 1, the virtual monitors at its exact resolution (the text
    // the same size on all of them) instead of the text size chosen.
    @Published var monitorsLikeMac: Bool { didSet { d.set(monitorsLikeMac, forKey: "monitorsLikeMac") } }
    @Published var growFocused: Bool { didSet { d.set(growFocused, forKey: "growFocused") } }  // pinned: the panel in use large
    // Pinned (⌘⌥G): the panel in use follows the mouse only, not where you look (Settings).
    @Published var growByGaze: Bool { didSet { d.set(growByGaze, forKey: "growByGaze") } }
    @Published var focusWindows: Bool { didSet { d.set(focusWindows, forKey: "focusWindows") } }  // floating windows count
    @Published var brightness: Int { didSet { d.set(brightness, forKey: "brightness") } }  // 1..8, 0 = leave
    @Published var k1: Double { didSet { d.set(k1, forKey: "k1") } }
    @Published var k2: Double { didSet { d.set(k2, forKey: "k2") } }

    @Published var onboardingDone: Bool { didSet { d.set(onboardingDone, forKey: "onboardingDone") } }

    init() {
        cells = Self.load([GridPos].self, "layoutCells")
            ?? [GridPos(col: 0, row: 0), GridPos(col: 1, row: 0)]
        panelPoses = Self.load([PanelPos].self, "panels") ?? []
        floating = (Self.load([FloatingWindow].self, "floatingWindows") ?? [])
            .map { var w = $0; w.windowID = 0; return w }  // ids of an earlier session
        textSize = d.object(forKey: "textSize") as? Int ?? 3
        distance = d.object(forKey: "distance") as? Double ?? 1.2
        gap = d.object(forKey: "gap") as? Double ?? 0.0
        curvature = d.object(forKey: "curvature") as? Double ?? (d.bool(forKey: "curved") ? 1 : 0)
        tilt = d.object(forKey: "tilt") as? Double ?? 0
        flatWall = d.object(forKey: "flatWall") as? Bool ?? true
        keyHighlight = d.object(forKey: "keyHighlight") as? Bool ?? true
        size = d.object(forKey: "size") as? Double ?? 38
        autoStart = d.bool(forKey: "autoStart")
        fov = d.object(forKey: "fovY") != nil ? Double(d.float(forKey: "fovY")) : 24.5
        let r = NormalMode.opticalVFovRange
        opticalVFov = Double(min(r.upperBound, max(r.lowerBound, Float(d.double(forKey: "opticalVFov") ?? Double(NormalMode.opticalVFovDeg)))))
        deadZone = d.object(forKey: "deadZone") as? Double ?? 1.0
        gazeCursor = d.object(forKey: "gazeCursor") as? Bool ?? true
        dimOthers = d.object(forKey: "dimOthers") as? Bool ?? false  // optical see-through: dimmed reads as cut off
        background = d.int(forKey: "background") ?? 0
        backgroundImage = d.object(forKey: "backgroundImage") as? String ?? ""
        backgroundBrightness = d.double(forKey: "backgroundBrightness") ?? 0.6
        gatherWindows = d.object(forKey: "gatherWindows") as? Bool ?? true
        tracking = d.object(forKey: "tracking") as? Int ?? ((d.object(forKey: "yawOnly") as? Bool ?? true) ? 1 : 0)
        // The space at real size, or the glasses' own pinned mode (⌘⌥G): as left last time.
        glassesPinned = d.int(forKey: "glassesMode") == 1
        growFocused = d.object(forKey: "growFocused") as? Bool ?? true
        growByGaze = d.object(forKey: "growByGaze") as? Bool ?? true
        macScreen = d.object(forKey: "macScreen") as? Bool ?? true
        monitorsLikeMac = d.object(forKey: "monitorsLikeMac") as? Bool ?? true
        anchorAtRest = d.bool(forKey: "anchorAtRest")
        vehicleMode = d.bool(forKey: "vehicleMode")
        focusWindows = d.object(forKey: "focusWindows") as? Bool ?? true
        // 0.015 was the first default: barely noticeable, so it moves to the new one.
        brightness = d.object(forKey: "brightness") as? Int ?? 0
        k1 = d.object(forKey: "k1") as? Double ?? 0
        k2 = d.object(forKey: "k2") as? Double ?? 0
        onboardingDone = d.bool(forKey: "onboardingDone")
        if !panelPoses.isEmpty { panelPoses = centred(panelPoses) }  // placed off centre before
        Self.adoptExistingCalibration()
    }

    // MARK: calibration file

    static var supportDirectory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("Nimbo", isDirectory: true)
        // Before the rename the app kept its files in "RayNeo".
        let legacy = base.appendingPathComponent("RayNeo", isDirectory: true)
        if !FileManager.default.fileExists(atPath: dir.path), FileManager.default.fileExists(atPath: legacy.path) {
            try? FileManager.default.copyItem(at: legacy, to: dir)
        }
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }


    static var calibrationURL: URL { supportDirectory.appendingPathComponent("calibration.json") }

    var hasCalibration: Bool { FileManager.default.fileExists(atPath: Self.calibrationURL.path) }

    // A calibration made earlier with imu_probe in the repository is reused.
    private static func adoptExistingCalibration() {
        let fm = FileManager.default
        guard !fm.fileExists(atPath: calibrationURL.path) else { return }
        let candidates = [fm.currentDirectoryPath + "/calibration.json",
                          NSHomeDirectory() + "/rayneo-gt-spatial-core/calibration.json"]
        if let src = candidates.first(where: { fm.fileExists(atPath: $0) }) {
            try? fm.copyItem(atPath: src, toPath: calibrationURL.path)
        }
    }

    // MARK: layout

    // Grid cells shifted so the top-left of the arrangement is (0, 0).
    var normalizedCells: [GridPos] {
        let c0 = cells.map(\.col).min() ?? 0, r0 = cells.map(\.row).min() ?? 0
        return cells.map { GridPos(col: $0.col - c0, row: $0.row - r0) }
    }

    func applyTemplate(_ t: [GridPos]) { cells = t }

    // MARK: hand placement (SpatialEditor)

    static let eyeAspect: Float = 16.0 / 9.0

    // Vertical size (deg) of a flat panel `arc` degrees wide (core::panel_vertical_arc).
    static func verticalArc(_ arc: Float) -> Float {
        2 * atan(tan(arc * .pi / 360) / eyeAspect) * 180 / .pi
    }

    // The grid as hand placements: where the monitors are now (core::grid_layout).
    func gridPoses() -> [PanelPos] {
        let n = normalizedCells
        let cols = n.map(\.col), rows = n.map(\.row)
        let midC = Float((cols.min() ?? 0) + (cols.max() ?? 0)) / 2, midR = Float((rows.min() ?? 0) + (rows.max() ?? 0)) / 2
        let w = Float(size) + Float(gap), h = Self.verticalArc(Float(size)) + Float(gap)
        return n.map { PanelPos(yaw: (Float($0.col) - midC) * w, pitch: (midR - Float($0.row)) * h) }
    }

    var effectivePoses: [PanelPos] { panelPoses.count == cells.count ? panelPoses : gridPoses() }

    // New hand placement; the OS arrangement (where the cursor crosses) follows the order the
    // monitors now have, like core::cells_from_poses.
    func place(_ poses: [PanelPos]) {
        panelPoses = centred(poses)
        let poses = panelPoses
        let cols = Self.clusters(poses.map(\.yaw), step: Float(size) / 2)
        let rows = Self.clusters(poses.map { -$0.pitch }, step: Self.verticalArc(Float(size)) / 2)
        var derived: [GridPos] = []
        for i in poses.indices {
            var c = GridPos(col: cols[i], row: rows[i])
            while derived.contains(c) { c.col += 1 }
            derived.append(c)
        }
        if derived != cells {
            keepingPoses = true
            cells = derived
            keepingPoses = false
        }
    }

    func resetPlacement() { panelPoses = [] }

    // The monitors' box centred straight ahead, as the glasses show them (core::poses_centre),
    // the floating windows moved with them: stacked by hand, the pair stayed where the dragged
    // one had been (19.5 deg to the right) and the editor showed them off centre.
    private func centred(_ poses: [PanelPos]) -> [PanelPos] {
        guard !poses.isEmpty else { return poses }
        let size = Float(self.size)
        let xs = poses.flatMap { [$0.yaw - size * $0.scale / 2, $0.yaw + size * $0.scale / 2] }
        let ys = poses.flatMap { p in [p.pitch - Self.verticalArc(size * p.scale) / 2, p.pitch + Self.verticalArc(size * p.scale) / 2] }
        let dx = ((xs.min() ?? 0) + (xs.max() ?? 0)) / 2, dy = ((ys.min() ?? 0) + (ys.max() ?? 0)) / 2
        guard abs(dx) > 0.01 || abs(dy) > 0.01 else { return poses }
        let moved = floating.map { w -> FloatingWindow in
            var w = w
            if var p = w.pose { p.yaw -= dx; p.pitch -= dy; w.pose = p }
            return w
        }
        if moved != floating { floating = moved }
        return poses.map { PanelPos(yaw: $0.yaw - dx, pitch: $0.pitch - dy, scale: $0.scale) }
    }

    private static func clusters(_ values: [Float], step: Float) -> [Int] {
        let order = values.indices.sorted { values[$0] < values[$1] }
        var out = [Int](repeating: 0, count: values.count)
        var c = 0
        for (k, i) in order.enumerated() {
            if k > 0, values[i] - values[order[k - 1]] > step { c += 1 }
            out[i] = c
        }
        return out
    }

    // MARK: one-click layouts and profiles

    struct Preset: Identifiable {
        let id: String
        let title: String
        let cells: [GridPos]
        let curvature: Double
        let size: Double
        let gap: Double
    }

    // Values come from the shared core (core/presets.h, same as Windows); titles are ours.
    private static let layoutTitles = ["curve": "1 Pantalla Curva", "dual": "Doble Monitor", "triple": "Triple Ultrawide"]

    static let layouts: [Preset] = (0..<rayneo_layout_preset_count()).compactMap { i in
        var p = rayneo_layout_preset()
        guard rayneo_layout_preset_at(i, &p), let raw = p.id else { return nil }
        let id = String(cString: raw)
        let n = Int(p.cell_count)
        let cols = withUnsafeBytes(of: p.cols) { Array($0.bindMemory(to: Int32.self).prefix(n)) }
        let rows = withUnsafeBytes(of: p.rows) { Array($0.bindMemory(to: Int32.self).prefix(n)) }
        return Preset(id: id, title: layoutTitles[id] ?? id,
                      cells: zip(cols, rows).map { GridPos(col: Int($0), row: Int($1)) },
                      curvature: Double(p.curvature), size: Double(p.arc_deg), gap: Double(p.gap_deg))
    }

    var activeLayout: String? {
        Self.layouts.first { $0.cells == normalizedCells }?.id
    }

    func apply(_ p: Preset) {
        cells = p.cells
        curvature = p.curvature
        size = p.size
        gap = p.gap
    }

    struct Profile: Identifiable {
        let id: String
        let title: String
        let symbol: String
        let layout: String
        let distance: Double
        let textSize: Int
        let curvature: Double?  // nil: keep the layout's
        let size: Double?
    }

    private static let profileLooks = ["code": ("Código", "chevron.left.forwardslash.chevron.right"),
                                       "cinema": ("Cine", "film")]

    static let profiles: [Profile] = (0..<rayneo_profile_count()).compactMap { i in
        var p = rayneo_profile()
        guard rayneo_profile_at(i, &p), let rawID = p.id, let rawLayout = p.layout_id else { return nil }
        let id = String(cString: rawID)
        let look = profileLooks[id] ?? (id, "square")
        return Profile(id: id, title: look.0, symbol: look.1, layout: String(cString: rawLayout),
                       distance: Double(p.distance_m), textSize: Int(p.text_size),
                       curvature: p.curvature >= 0 ? Double(p.curvature) : nil,
                       size: p.arc_deg >= 0 ? Double(p.arc_deg) : nil)
    }

    // Adds a monitor touching the arrangement: right of a row first, then below a column,
    // keeping the whole layout within 4 columns x 3 rows.
    func addMonitor() {
        guard cells.count < 6 else { return }
        let occupied = Set(cells)
        let rows = Set(cells.map(\.row)).sorted(), cols = Set(cells.map(\.col)).sorted()
        var candidates: [GridPos] = []
        for r in rows { candidates.append(GridPos(col: cells.filter { $0.row == r }.map(\.col).max()! + 1, row: r)) }
        for c in cols { candidates.append(GridPos(col: c, row: cells.filter { $0.col == c }.map(\.row).max()! + 1)) }
        let fits: (GridPos) -> Bool = { p in
            let all = self.cells + [p]
            let spanC = (all.map(\.col).max()! - all.map(\.col).min()!) + 1
            let spanR = (all.map(\.row).max()! - all.map(\.row).min()!) + 1
            return spanC <= 4 && spanR <= 3
        }
        if let next = candidates.first(where: { !occupied.contains($0) && fits($0) }) { cells.append(next) }
    }

    func removeMonitor() {
        guard cells.count > 1 else { return }
        var next = cells
        next.removeLast()
        if Self.isConnected(next) { cells = next }
    }

    // Monitors must touch edge to edge, as macOS requires for an arrangement.
    static func isConnected(_ cells: [GridPos]) -> Bool {
        guard let first = cells.first else { return true }
        let all = Set(cells)
        var seen: Set<GridPos> = [first]
        var stack = [first]
        while let c = stack.popLast() {
            for n in [GridPos(col: c.col + 1, row: c.row), GridPos(col: c.col - 1, row: c.row),
                      GridPos(col: c.col, row: c.row + 1), GridPos(col: c.col, row: c.row - 1)]
            where all.contains(n) && !seen.contains(n) {
                seen.insert(n)
                stack.append(n)
            }
        }
        return seen.count == all.count && all.count == cells.count
    }

    // MARK: server options

    func serverOptions() -> ServerOptions {
        var o = ServerOptions()
        o.headless = false
        o.layout = normalizedCells
        o.panelPoses = panelPoses.count == cells.count ? panelPoses : nil
        o.floatingWindows = floating
        o.displays = cells.count
        let resolution = Self.textSizes[max(0, min(Self.textSizes.count - 1, textSize))]
        o.vdWidth = resolution.width
        o.vdHeight = resolution.height
        o.vdLikeMac = monitorsLikeMac
        o.distanceM = Float(distance)
        o.gapDeg = Float(gap)
        o.curvature = Float(curvature)
        o.tilt = Float(tilt)
        o.arcDeg = Float(size)
        // The space at real size: the optics' field of view, late latching and a fixed prediction.
        o.fovYDeg = glassesPinned ? Float(fov) : Float(opticalVFov)
        o.lateLatch = !glassesPinned
        o.predictionMs = glassesPinned ? 0 : NormalMode.predictionMs
        o.supersample = glassesPinned ? 1 : NormalMode.supersample
        o.fovFromCLI = true
        // Space at real size: the panels never move with the head, not even a little (the dead
        // zone would hold them on the head while it moves less than it, then let go).
        // Anchor at rest: frozen while the head holds within 1 deg sideways (-1 - 1, see
        // rayneo_config; 2 deg up / down): above breathing and pulse sway (0.5-0.8 deg); at 0.3 it
        // let go every few seconds and the panels trembled with the head, at 0.6 still now and then.
        o.deadZoneDeg = glassesPinned ? Float(deadZone) : (anchorAtRest ? -2 : -1)
        o.gazeCursor = gazeCursor
        o.dimOthers = dimOthers ? 0.55 : 1  // linear light: ~80 % perceived brightness
        o.background = background
        o.backgroundImage = backgroundImage
        o.backgroundBrightness = Float(backgroundBrightness)
        o.gatherWindows = gatherWindows
        o.trackingAxes = tracking
        o.vehicleMode = vehicleMode && !glassesPinned
        o.flatWall = flatWall
        o.keyHighlight = keyHighlight
        o.glassesPinned = glassesPinned
        o.recenterGlasses = glassesPinned
        // The two ways of seeing the panels set the glasses' mode themselves: pinned for one
        // panel large in their fixed image, follow for the space at real size.
        o.glassesMode = glassesPinned ? 1 : 0
        o.realSize = !glassesPinned
        o.growFocused = growFocused
        o.growByGaze = growByGaze
        o.macScreen = macScreen
        o.focusWindows = focusWindows
        o.zoom = Float(d.double(forKey: "zoom") ?? 1)  // as left last time (⌘⌥+ / - / 0, ⌘⌥K)
        o.brightness = brightness
        o.unmirror = true  // mirrored glasses run at the Mac's resolution and composited (reverted on exit)
        o.k1 = Float(k1)
        o.k2 = Float(k2)
        o.calibrationPath = Self.calibrationURL.path
        if ProcessInfo.processInfo.environment["RAYNEO_SIM"] != nil {
            o.sim = true
            o.windowed = true
        }
        // Per-app profile on top (transient: the saved settings stay as they are).
        return o
    }

    // MARK: persistence

    private func save<T: Encodable>(_ v: T, _ key: String) { d.encode(v, forKey: key) }

    private static func load<T: Decodable>(_ t: T.Type, _ key: String) -> T? {
        SettingsFile.shared.decode(t, forKey: key)
    }
}
