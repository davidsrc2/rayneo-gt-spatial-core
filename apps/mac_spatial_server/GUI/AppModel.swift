import AppKit
import Combine
import Foundation

// Every user-facing setting, persisted in UserDefaults, and how it maps to ServerOptions.
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

    private let d = UserDefaults.standard

    @Published var cells: [GridPos] { didSet { save(cells, "layoutCells") } }
    @Published var textSize: Int { didSet { d.set(textSize, forKey: "textSize") } }
    @Published var distance: Double { didSet { d.set(distance, forKey: "distance") } }
    @Published var gap: Double { didSet { d.set(gap, forKey: "gap") } }
    @Published var curved: Bool { didSet { d.set(curved, forKey: "curved") } }

    // Advanced
    @Published var fov: Double { didSet { d.set(Float(fov), forKey: "fovY") } }
    @Published var ipd: Double { didSet { d.set(ipd, forKey: "ipd") } }
    @Published var stereo: Bool { didSet { d.set(stereo, forKey: "stereo") } }
    @Published var deadZone: Double { didSet { d.set(deadZone, forKey: "deadZone") } }
    @Published var gazeCursor: Bool { didSet { d.set(gazeCursor, forKey: "gazeCursor") } }
    @Published var gatherWindows: Bool { didSet { d.set(gatherWindows, forKey: "gatherWindows") } }
    @Published var k1: Double { didSet { d.set(k1, forKey: "k1") } }
    @Published var k2: Double { didSet { d.set(k2, forKey: "k2") } }

    @Published var onboardingDone: Bool { didSet { d.set(onboardingDone, forKey: "onboardingDone") } }

    init() {
        cells = Self.load([GridPos].self, "layoutCells", from: UserDefaults.standard)
            ?? [GridPos(col: 0, row: 0), GridPos(col: 1, row: 0)]
        textSize = d.object(forKey: "textSize") as? Int ?? 3
        distance = d.object(forKey: "distance") as? Double ?? 2.0
        gap = d.object(forKey: "gap") as? Double ?? 1.0
        curved = d.bool(forKey: "curved")
        fov = d.object(forKey: "fovY") != nil ? Double(d.float(forKey: "fovY")) : 24.5
        ipd = d.object(forKey: "ipd") as? Double ?? 63
        stereo = d.bool(forKey: "stereo")
        deadZone = d.object(forKey: "deadZone") as? Double ?? 1.0
        gazeCursor = d.object(forKey: "gazeCursor") as? Bool ?? true
        gatherWindows = d.object(forKey: "gatherWindows") as? Bool ?? true
        k1 = d.object(forKey: "k1") as? Double ?? 0
        k2 = d.object(forKey: "k2") as? Double ?? 0
        onboardingDone = d.bool(forKey: "onboardingDone")
        Self.adoptExistingCalibration()
    }

    // MARK: calibration file

    static var supportDirectory: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RayNeo", isDirectory: true)
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
        o.displays = cells.count
        let size = Self.textSizes[max(0, min(Self.textSizes.count - 1, textSize))]
        o.vdWidth = size.width
        o.vdHeight = size.height
        o.distanceM = Float(distance)
        o.gapDeg = Float(gap)
        o.curved = curved
        o.fovYDeg = Float(fov)
        o.fovFromCLI = true
        o.ipdMM = Float(ipd)
        o.sideBySide = stereo
        o.deadZoneDeg = Float(deadZone)
        o.gazeCursor = gazeCursor
        o.gatherWindows = gatherWindows
        o.k1 = Float(k1)
        o.k2 = Float(k2)
        o.calibrationPath = Self.calibrationURL.path
        if ProcessInfo.processInfo.environment["RAYNEO_SIM"] != nil {
            o.sim = true
            o.windowed = true
        }
        return o
    }

    // MARK: persistence

    private func save<T: Encodable>(_ v: T, _ key: String) {
        if let data = try? JSONEncoder().encode(v) { d.set(data, forKey: key) }
    }

    private static func load<T: Decodable>(_ t: T.Type, _ key: String, from d: UserDefaults) -> T? {
        guard let data = d.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(t, from: data)
    }
}
