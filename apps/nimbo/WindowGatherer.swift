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
