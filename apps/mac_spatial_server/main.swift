// mac_spatial_server: captures the desktop with ScreenCaptureKit and renders it on a
// curved virtual screen anchored in space, half-SBS on the RayNeo GT display.

import AppKit

guard let options = ServerOptions.parse(CommandLine.arguments) else {
    print(ServerOptions.usage)
    exit(1)
}
guard #available(macOS 14.0, *) else {
    print("macOS 14 or newer required (CAMetalDisplayLink)")
    exit(1)
}

// One server at a time: two would fight over the display arrangement, and the first to quit
// would revert the other's (app-scoped) configuration.
let lockPath = NSTemporaryDirectory() + "rayneo-spatial-server.lock"
let lockFD = open(lockPath, O_CREAT | O_RDWR, 0o644)
if lockFD < 0 || flock(lockFD, LOCK_EX | LOCK_NB) != 0 {
    print("another mac_spatial_server is already running: quit it first (Cmd+Opt+Q)")
    exit(1)
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let options: ServerOptions
    var server: AnyObject?

    init(options: ServerOptions) {
        self.options = options
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard #available(macOS 14.0, *) else { return }
        let s = SpatialServer(options: options)
        server = s
        do {
            try s.start()
        } catch {
            FileHandle.standardError.write("start failed: \(error.localizedDescription)\n".data(using: .utf8)!)
            exit(1)
        }
        if options.durationS > 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + options.durationS) { NSApp.terminate(nil) }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        if #available(macOS 14.0, *), let s = server as? SpatialServer { s.stop() }
    }
}

// Ctrl+C from the terminal: terminate cleanly so stream-off is sent.
signal(SIGINT, SIG_IGN)
let sigint = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
sigint.setEventHandler { NSApp.terminate(nil) }
sigint.resume()

let app = NSApplication.shared
let delegate = AppDelegate(options: options)
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
