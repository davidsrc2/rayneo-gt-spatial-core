import AppKit
import CoreGraphics

enum Displays {
    static func id(of screen: NSScreen) -> CGDirectDisplayID {
        (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
    }

    // The RayNeo GT enumerates as "SmartGlasses" (vendor 0x506c on the reference unit).
    // Our own virtual monitors ("Nimbo N", vendor 'RN') are never the glasses.
    static func glassesScreen() -> NSScreen? {
        NSScreen.screens.first { s in
            let id = id(of: s)
            if CGDisplayVendorNumber(id) == 0x524E { return false }
            let name = s.localizedName.lowercased()
            if name.hasPrefix("nimbo") || name.hasPrefix("rayneo panel") { return false }
            return name.contains("smartglasses") || name.contains("rayneo") || CGDisplayVendorNumber(id) == 0x506c
        }
    }

    // The display's area without the menu bar (and Dock), in global top-left coordinates
    // (CGDisplayBounds space).
    static func visibleBounds(_ display: CGDirectDisplayID) -> CGRect? {
        guard let screen = NSScreen.screens.first(where: { id(of: $0) == display }),
              let primary = NSScreen.screens.first else { return nil }
        let v = screen.visibleFrame
        return CGRect(x: v.minX, y: primary.frame.height - v.maxY, width: v.width, height: v.height)
    }

    // The Mac's own screen when it is on (lid open) and not just mirroring another display.
    static func builtIn() -> CGDirectDisplayID? {
        var n: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &n) == .success, n > 0 else { return nil }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(n))
        guard CGGetActiveDisplayList(n, &ids, &n) == .success else { return nil }
        return ids.prefix(Int(n)).first { CGDisplayIsBuiltin($0) != 0 && CGDisplayMirrorsDisplay($0) == kCGNullDirectDisplay }
    }

    static func isMirrored(_ display: CGDirectDisplayID) -> Bool {
        CGDisplayIsInMirrorSet(display) != 0
    }

    // App-scoped: take every other display out of the glasses' mirror set (reverted on
    // exit). Mirroring forces WindowServer composition on the glasses output.
    static func unmirror(glasses: CGDirectDisplayID) -> Bool {
        var n: UInt32 = 0
        CGGetOnlineDisplayList(0, nil, &n)
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(n))
        CGGetOnlineDisplayList(n, &ids, &n)
        let mirrored = ids.filter { $0 != glasses && (CGDisplayMirrorsDisplay($0) == glasses || CGDisplayMirrorsDisplay(glasses) == $0) }
        guard !mirrored.isEmpty || CGDisplayMirrorsDisplay(glasses) != kCGNullDirectDisplay else { return true }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return false }
        for id in mirrored { CGConfigureDisplayMirrorOfDisplay(config, id, kCGNullDirectDisplay) }
        CGConfigureDisplayMirrorOfDisplay(config, glasses, kCGNullDirectDisplay)
        return CGCompleteDisplayConfiguration(config, .forAppOnly) == .success
    }

    // App-scoped: native 1:1 mode (e.g. 1920x1080 px at 1x) at `hz` if available. The user's
    // "larger text" HiDPI mode is a scaled 2560x1440 surface the glasses downsample; while
    // the server runs, the display is only a render target, so render natively. Reverted on
    // exit, restoring the user's setting.
    static func setNative(_ display: CGDirectDisplayID, hz: Double) -> CGDisplayMode? {
        let opts = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
        let modes = (CGDisplayCopyAllDisplayModes(display, opts) as? [CGDisplayMode]) ?? []
        let native = modes.filter { $0.pixelWidth == $0.width && $0.isUsableForDesktopGUI() }
        guard let maxW = native.map(\.pixelWidth).max() else { return nil }
        let candidates = native.filter { $0.pixelWidth == maxW }
        guard let mode = candidates.first(where: { $0.refreshRate == hz }) ?? candidates.max(by: { $0.refreshRate < $1.refreshRate })
        else { return nil }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return nil }
        CGConfigureDisplayWithDisplayMode(config, display, mode, nil)
        return CGCompleteDisplayConfiguration(config, .forAppOnly) == .success ? mode : nil
    }

    // App-scoped mode switch (reverted automatically when the process exits).
    static func setRefresh(_ display: CGDirectDisplayID, hz: Double) -> Bool {
        guard let current = CGDisplayCopyDisplayMode(display) else { return false }
        if current.refreshRate == hz { return true }
        let opts = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
        let modes = (CGDisplayCopyAllDisplayModes(display, opts) as? [CGDisplayMode]) ?? []
        guard let mode = modes.first(where: {
            $0.pixelWidth == current.pixelWidth && $0.pixelHeight == current.pixelHeight
                && $0.width == current.width && $0.refreshRate == hz
        }) else { return false }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return false }
        CGConfigureDisplayWithDisplayMode(config, display, mode, nil)
        return CGCompleteDisplayConfiguration(config, .forAppOnly) == .success
    }
}
