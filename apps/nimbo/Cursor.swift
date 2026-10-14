import CoreGraphics
import Darwin

// The glasses show the main display's captured cursor inside its (world-anchored) panel,
// but WindowServer also draws the hardware cursor on top of the full-screen render window,
// head-locked: two cursors. Hide the hardware one.
//
// CGDisplayHideCursor only takes effect for the active app; a background app needs the
// private "SetsCursorInBackground" connection property (used by e.g. Cursorcerer).
enum HardwareCursor {
    private typealias DefaultConnection = @convention(c) () -> Int32
    private typealias SetProperty = @convention(c) (Int32, Int32, CFString, CFTypeRef) -> Int32

    private static var hidden = false
    static var isHidden: Bool { hidden }

    static func hide() -> Bool {
        guard !hidden else { return true }
        let cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY)
        if let cg, let c = dlsym(cg, "_CGSDefaultConnection"), let s = dlsym(cg, "CGSSetConnectionProperty") {
            let conn = unsafeBitCast(c, to: DefaultConnection.self)()
            _ = unsafeBitCast(s, to: SetProperty.self)(conn, conn, "SetsCursorInBackground" as CFString, kCFBooleanTrue)
        }
        hidden = CGDisplayHideCursor(CGMainDisplayID()) == .success
        return hidden
    }

    static func show() {
        guard hidden else { return }
        CGDisplayShowCursor(CGMainDisplayID())
        hidden = false
    }
}
