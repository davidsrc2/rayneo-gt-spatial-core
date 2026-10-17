import Foundation

// The settings in ~/Library/Application Support/Nimbo/settings.json, the same file format and
// keys as Windows' %LOCALAPPDATA%\Nimbo\settings.json (core/settings.h): a file can be copied
// from one computer to the other. Keys only the Mac knows (per-app profiles, floating windows)
// sit alongside; Windows ignores them.
//
// UserDefaults-like accessors. Writes are batched and land on disk a moment later (and when
// the app quits). The first time, the settings kept in UserDefaults by earlier builds are
// copied over (UserDefaults is left as it was).
final class SettingsFile: @unchecked Sendable {
    static let shared = SettingsFile(url: FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Nimbo/settings.json"))

    let url: URL
    private let lock = NSLock()
    private var values: [String: Any] = [:]  // guarded by lock
    private var dirty = false                // guarded by lock
    private var flushScheduled = false       // guarded by lock

    init(url: URL) {
        self.url = url
        if let data = try? Data(contentsOf: url),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            values = object
        } else if !FileManager.default.fileExists(atPath: url.path) {
            values = Self.fromUserDefaults()
            if !values.isEmpty {
                dirty = true
                flush()
            }
        }
        // A file that exists but is not JSON is left untouched until something is saved.
    }

    // MARK: reading

    func object(forKey key: String) -> Any? {
        lock.lock()
        defer { lock.unlock() }
        return values[key]
    }

    func bool(forKey key: String) -> Bool { (object(forKey: key) as? NSNumber)?.boolValue ?? false }
    func float(forKey key: String) -> Float { (object(forKey: key) as? NSNumber)?.floatValue ?? 0 }
    func double(forKey key: String) -> Double? { (object(forKey: key) as? NSNumber)?.doubleValue }
    func int(forKey key: String) -> Int? { (object(forKey: key) as? NSNumber)?.intValue }

    // A JSON array or object under `key`, decoded.
    func decode<T: Decodable>(_ type: T.Type, forKey key: String) -> T? {
        guard let v = object(forKey: key), JSONSerialization.isValidJSONObject(v),
              let data = try? JSONSerialization.data(withJSONObject: v) else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    // MARK: writing

    func set(_ value: Any?, forKey key: String) {
        lock.lock()
        values[key] = value
        dirty = true
        let schedule = !flushScheduled
        flushScheduled = true
        lock.unlock()
        // A slider sends many values in a row: one write a moment after the last.
        if schedule {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in self?.flush() }
        }
    }

    // Stored as JSON (an array of {col, row}..., not an opaque blob).
    func encode<T: Encodable>(_ value: T, forKey key: String) {
        guard let data = try? JSONEncoder().encode(value),
              let object = try? JSONSerialization.jsonObject(with: data) else { return }
        set(object, forKey: key)
    }

    // Writes pending changes now (also at quit).
    func flush() {
        lock.lock()
        flushScheduled = false
        guard dirty else {
            lock.unlock()
            return
        }
        dirty = false
        let snapshot = values
        lock.unlock()
        guard let data = try? JSONSerialization.data(withJSONObject: snapshot, options: [.prettyPrinted, .sortedKeys])
        else { return }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: url, options: .atomic)
    }

    // MARK: migration

    // Settings of earlier builds (UserDefaults), with the names and shapes of the file: JSON
    // arrays instead of encoded blobs, "panelPoses" as "panels".
    private static let plainKeys = [
        "textSize", "distance", "gap", "curvature", "curved", "tilt", "size", "autoStart", "fovY", "ipd", "stereo",
        "deadZone", "gazeCursor", "dimOthers", "gatherWindows", "tracking", "yawOnly", "glassesMode", "anchorAtRest",
        "macScreen", "zoom", "growFocused", "growByGaze", "focusWindows", "brightness", "depth3D", "depthStrength", "depthPlane", "k1", "k2",
        "onboardingDone",
    ]
    private static let blobKeys = ["layoutCells": "layoutCells", "panelPoses": "panels",
                                   "appProfiles": "appProfiles", "floatingWindows": "floatingWindows"]

    private static func fromUserDefaults() -> [String: Any] {
        let d = UserDefaults.standard
        var out: [String: Any] = [:]
        for key in plainKeys {
            if let v = d.object(forKey: key) as? NSNumber { out[key] = v }
        }
        for (old, new) in blobKeys {
            if let data = d.data(forKey: old), let object = try? JSONSerialization.jsonObject(with: data) {
                out[new] = object
            }
        }
        return out
    }
}
