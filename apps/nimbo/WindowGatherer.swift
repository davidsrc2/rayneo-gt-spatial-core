import AppKit
import ApplicationServices

// Moves windows that sit on the glasses' physical display (invisible once it is only a
// render surface) onto a virtual monitor. Uses the Accessibility API: the app that launched
// the server (e.g. Terminal) needs Privacy & Security > Accessibility.
enum WindowGatherer {
    static func isTrusted(prompt: Bool) -> Bool {
        let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        return AXIsProcessTrustedWithOptions([key: prompt] as CFDictionary)
    }

    // Global coordinates (top-left origin, CGDisplayBounds space). Returns windows moved.
    static func gather(from source: CGRect, into target: CGRect) -> Int {
        var moved = 0
        let me = getpid()
        for app in NSWorkspace.shared.runningApplications where app.activationPolicy == .regular && app.processIdentifier != me {
            let axApp = AXUIElementCreateApplication(app.processIdentifier)
            var value: CFTypeRef?
            guard AXUIElementCopyAttributeValue(axApp, kAXWindowsAttribute as CFString, &value) == .success,
                  let windows = value as? [AXUIElement] else { continue }
            for w in windows {
                guard let pos = point(w, kAXPositionAttribute), let size = size(w, kAXSizeAttribute) else { continue }
                let center = CGPoint(x: pos.x + size.width / 2, y: pos.y + size.height / 2)
                guard source.contains(center) else { continue }
                // Same relative placement, shrunk to fit the (usually smaller) target.
                let fit = CGSize(width: min(size.width, target.width * 0.9), height: min(size.height, target.height * 0.85))
                let rx = (pos.x - source.minX) / max(1, source.width - size.width)
                let ry = (pos.y - source.minY) / max(1, source.height - size.height)
                var newPos = CGPoint(x: target.minX + max(0, min(1, rx)) * (target.width - fit.width),
                                     y: target.minY + 30 + max(0, min(1, ry)) * (target.height - 30 - fit.height))
                var newSize = fit
                if let p = AXValueCreate(.cgPoint, &newPos), let s = AXValueCreate(.cgSize, &newSize) {
                    AXUIElementSetAttributeValue(w, kAXSizeAttribute as CFString, s)
                    if AXUIElementSetAttributeValue(w, kAXPositionAttribute as CFString, p) == .success { moved += 1 }
                }
            }
        }
        return moved
    }

    // A window's frame in global top-left coordinates (CGWindowList: no Accessibility needed).
    static func frame(windowID: CGWindowID) -> CGRect? {
        guard let list = CGWindowListCopyWindowInfo([.optionIncludingWindow], windowID) as? [[String: Any]],
              let b = list.first?[kCGWindowBounds as String] as? NSDictionary else { return nil }
        return CGRect(dictionaryRepresentation: b as CFDictionary)
    }

    // The accessibility element of a window (found by its current frame, else its title; with
    // `orFocused`, else its app's focused window: a window in full screen in a Space of its own
    // may report neither).
    private static func axWindow(windowID: CGWindowID, title: String, orFocused: Bool = false) -> AXUIElement? {
        guard let list = CGWindowListCopyWindowInfo([.optionIncludingWindow], windowID) as? [[String: Any]],
              let pid = list.first?[kCGWindowOwnerPID as String] as? pid_t else { return nil }
        let current = frame(windowID: windowID)
        let axApp = AXUIElementCreateApplication(pid)
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(axApp, kAXWindowsAttribute as CFString, &value) == .success,
              let windows = value as? [AXUIElement] else { return nil }
        let placed = windows.first { w in
            guard let current, let p = point(w, kAXPositionAttribute), let s = size(w, kAXSizeAttribute) else { return false }
            return abs(p.x - current.minX) < 2 && abs(p.y - current.minY) < 2 && abs(s.width - current.width) < 2
        }
        let titled = windows.first { w in
            var t: CFTypeRef?
            return !title.isEmpty && AXUIElementCopyAttributeValue(w, kAXTitleAttribute as CFString, &t) == .success && (t as? String) == title
        }
        if let w = placed ?? titled { return w }
        guard orFocused else { return nil }
        var focused: CFTypeRef?
        guard AXUIElementCopyAttributeValue(axApp, kAXFocusedWindowAttribute as CFString, &focused) == .success,
              let focused, CFGetTypeID(focused) == AXUIElementGetTypeID() else { return nil }
        return (focused as! AXUIElement)
    }

    // The window in macOS full screen (a Space of its own), and taking it out of it.
    static func isFullScreen(windowID: CGWindowID, title: String, orFocused: Bool = false) -> Bool {
        guard let w = axWindow(windowID: windowID, title: title, orFocused: orFocused) else { return false }
        var v: CFTypeRef?
        return AXUIElementCopyAttributeValue(w, "AXFullScreen" as CFString, &v) == .success && (v as? Bool) == true
    }

    @discardableResult
    static func exitFullScreen(windowID: CGWindowID, title: String, orFocused: Bool = false) -> Bool {
        guard let w = axWindow(windowID: windowID, title: title, orFocused: orFocused) else { return false }
        return AXUIElementSetAttributeValue(w, "AXFullScreen" as CFString, kCFBooleanFalse) == .success
    }

    // Moves one window (found by its owner and title, or by its current frame) to fill `target`.
    static func move(windowID: CGWindowID, title: String, to target: CGRect, orFocused: Bool = false) -> Bool {
        guard let w = axWindow(windowID: windowID, title: title, orFocused: orFocused) else { return false }
        var pos = target.origin
        var sz = target.size
        guard let p = AXValueCreate(.cgPoint, &pos), let s = AXValueCreate(.cgSize, &sz) else { return false }
        // Position first (onto the new monitor), then the size it allows, then position again
        // in case the size was clamped on the way.
        AXUIElementSetAttributeValue(w, kAXPositionAttribute as CFString, p)
        AXUIElementSetAttributeValue(w, kAXSizeAttribute as CFString, s)
        return AXUIElementSetAttributeValue(w, kAXPositionAttribute as CFString, p) == .success
    }

    private static func point(_ e: AXUIElement, _ attr: String) -> CGPoint? {
        var v: CFTypeRef?
        guard AXUIElementCopyAttributeValue(e, attr as CFString, &v) == .success, let v,
              CFGetTypeID(v) == AXValueGetTypeID() else { return nil }
        var p = CGPoint.zero
        return AXValueGetValue(v as! AXValue, .cgPoint, &p) ? p : nil
    }

    private static func size(_ e: AXUIElement, _ attr: String) -> CGSize? {
        var v: CFTypeRef?
        guard AXUIElementCopyAttributeValue(e, attr as CFString, &v) == .success, let v,
              CFGetTypeID(v) == AXValueGetTypeID() else { return nil }
        var s = CGSize.zero
        return AXValueGetValue(v as! AXValue, .cgSize, &s) ? s : nil
    }
}
