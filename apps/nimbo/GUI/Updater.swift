import AppKit
import CryptoKit
import Security

// Updates from the repository's GitHub Releases, as on Windows: the latest release's
// Nimbo-macOS-<version>.zip, checked against the SHA-256 GitHub publishes for it and against
// this app's own signature (same certificate: macOS keeps the Screen Recording and
// Accessibility permissions), then swapped in place of this app, which opens again.
// Downloaded by the app itself the new copy carries no quarantine: no Gatekeeper prompt.
@available(macOS 14.0, *)
final class Updater: ObservableObject {
    struct Release {
        let version: String
        let url: URL
        let sha256: String
    }

    static let shared = Updater()

    @Published private(set) var available: Release?
    @Published private(set) var status = ""
    @Published private(set) var busy = false
    private var relaunching = false  // the helper is waiting for us to quit: never a second one
    var onFound: ((Release) -> Void)?

    private static let latestAPI = URL(string: "https://api.github.com/repos/davidsrc2/rayneo-gt-spatial-core/releases/latest")!
    private var timer: Timer?

    static var current: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.0.0"
    }

    // A look 20 s after start, then every 12 h.
    func start() {
        status = "Versión \(Self.current)"
        timer = Timer.scheduledTimer(withTimeInterval: 20, repeats: false) { [weak self] _ in
            Task { await self?.check(quiet: true) }
            self?.timer = Timer.scheduledTimer(withTimeInterval: 12 * 3600, repeats: true) { [weak self] _ in
                Task { await self?.check(quiet: true) }
            }
        }
    }

    @MainActor
    func check(quiet: Bool) async {
        if !quiet { status = "Buscando…" }
        let before = available?.version
        if let r = await Self.fetchLatest() { available = r }
        if let r = available {
            status = "Versión \(Self.current). Hay una nueva: \(r.version)."
            if r.version != before { onFound?(r) }
        } else {
            status = quiet ? "Versión \(Self.current)" : "Versión \(Self.current): es la última."
        }
    }

    @MainActor
    func install() async {
        guard let r = available, !busy, !relaunching else { return }
        busy = true
        defer { busy = false }
        status = "Descargando la actualización…"
        do {
            let (file, _) = try await URLSession.shared.download(from: r.url)
            let data = try Data(contentsOf: file)
            let hex = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
            guard hex == r.sha256 else {
                status = "La descarga no coincide con la publicada; no se ha instalado."
                return
            }
            let dir = FileManager.default.temporaryDirectory.appendingPathComponent("Nimbo-Update-\(r.version)")
            try? FileManager.default.removeItem(at: dir)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let zip = dir.appendingPathComponent("Nimbo.zip")
            try data.write(to: zip)
            guard Self.run("/usr/bin/ditto", ["-x", "-k", zip.path, dir.path]) == 0 else {
                status = "No se pudo descomprimir la actualización."
                return
            }
            let app = dir.appendingPathComponent("Nimbo.app")
            guard Self.signedLikeMe(app) else {
                status = "La actualización no está firmada como esta app; no se ha instalado (compila con la firma de Nimbo)."
                return
            }
            try relaunchInto(app)
        } catch {
            status = "No se pudo descargar la actualización."
        }
    }

    // A helper shell waits for this app to quit, swaps the bundle and opens the new one.
    @MainActor
    private func relaunchInto(_ app: URL) throws {
        let dest = Bundle.main.bundlePath
        let script = """
        while kill -0 \(getpid()) 2>/dev/null; do sleep 0.2; done
        rm -rf "$1.old" && mv "$1" "$1.old" && /usr/bin/ditto "$2" "$1" && rm -rf "$1.old" || { [ -d "$1.old" ] && [ ! -d "$1" ] && mv "$1.old" "$1"; }
        /usr/bin/xattr -dr com.apple.quarantine "$1" 2>/dev/null
        /usr/bin/open "$1"
        """
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", script, "nimbo-update", dest, app.path]
        try p.run()
        relaunching = true
        status = "Instalando…"
        // AppKit ignores terminate while a sheet is up (Ajustes is one): close them first, and
        // if the app still has not quit in 3 s, the same clean-up as a normal quit, then exit.
        for w in NSApp.windows {
            if let sheet = w.attachedSheet { w.endSheet(sheet) }
        }
        NSApp.terminate(nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            NSApp.delegate?.applicationWillTerminate?(Notification(name: NSApplication.willTerminateNotification))
            exit(0)
        }
    }

    private static func fetchLatest() async -> Release? {
        var req = URLRequest(url: latestAPI)
        req.setValue("Nimbo-Updater", forHTTPHeaderField: "User-Agent")
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        guard let (data, resp) = try? await URLSession.shared.data(for: req),
              (resp as? HTTPURLResponse)?.statusCode == 200,
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (root["draft"] as? Bool) == false, (root["prerelease"] as? Bool) == false,
              let tag = root["tag_name"] as? String else { return nil }
        let version = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
        guard newer(version, than: current) else { return nil }
        for a in root["assets"] as? [[String: Any]] ?? [] {
            guard let name = a["name"] as? String, name.hasPrefix("Nimbo-macOS-"), name.hasSuffix(".zip"),
                  let link = a["browser_download_url"] as? String, let url = URL(string: link),
                  let digest = a["digest"] as? String, digest.hasPrefix("sha256:") else { continue }
            return Release(version: version, url: url, sha256: String(digest.dropFirst(7)).lowercased())
        }
        return nil
    }

    static func newer(_ a: String, than b: String) -> Bool {
        let x = a.split(separator: ".").map { Int($0) ?? 0 }, y = b.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(x.count, y.count) {
            let p = i < x.count ? x[i] : 0, q = i < y.count ? y[i] : 0
            if p != q { return p > q }
        }
        return false
    }

    // The new bundle satisfies this app's designated requirement (bundle id + certificate).
    private static func signedLikeMe(_ app: URL) -> Bool {
        var me: SecCode?
        var requirement: SecRequirement?
        var staticMe: SecStaticCode?
        guard SecCodeCopySelf([], &me) == errSecSuccess, let me,
              SecCodeCopyStaticCode(me, [], &staticMe) == errSecSuccess, let staticMe,
              SecCodeCopyDesignatedRequirement(staticMe, [], &requirement) == errSecSuccess, let requirement else { return false }
        var other: SecStaticCode?
        guard SecStaticCodeCreateWithPath(app as CFURL, [], &other) == errSecSuccess, let other else { return false }
        return SecStaticCodeCheckValidity(other, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), requirement) == errSecSuccess
    }

    private static func run(_ tool: String, _ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        guard (try? p.run()) != nil else { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }
}
