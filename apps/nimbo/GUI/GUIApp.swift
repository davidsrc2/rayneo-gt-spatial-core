import AppKit
import Carbon.HIToolbox
import SwiftUI

// The app when opened normally (no command-line flags): one calm window plus "◎" in the
// menu bar. The server runs in-process and is started/stopped from either.
@available(macOS 14.0, *)
final class GUIApp: NSObject {
    let model = AppModel()
    let server = ServerController()
    private var window: NSWindow?
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private var panel: KeyPanel?
    private let panelLayout = PanelLayout()
    private var panelPlacing = false  // our own setFrame: not a move by the user
    private static let panelTopLeftKey = "quickPanelTopLeft"
    private let hotKeys = HotKeys()

    func launch() {
        NSApp.setActivationPolicy(.accessory)  // a menu bar app: no Dock icon
        NSApp.appearance = NSAppearance(named: .darkAqua)
        server.bind(model)
        buildStatusItem()
        registerLayoutHotKeys()
        watchFrontApp()
        if ProcessInfo.processInfo.environment["RAYNEO_PANEL_PREVIEW"] != nil {
            showPanelPreviewWindow()  // development: the panel as a plain window, for screenshots
            return
        }
        if model.onboardingDone {
            DispatchQueue.main.async { self.showPanel() }
        } else {
            openLayout()  // first run: the full window hosts the welcome assistant
        }
    }

    func shutdown() { server.stop() }

    // Per-app profiles: the front app decides which profile's look applies (Nimbo itself in
    // front, e.g. its panel, keeps the current one).
    private var frontAppObserver: NSObjectProtocol?

    private func watchFrontApp() {
        frontAppObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
        ) { [weak self] note in
            guard let self,
                  let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication,
                  app.bundleIdentifier != Bundle.main.bundleIdentifier else { return }
            let profile = self.model.appRules.first { $0.bundleID == app.bundleIdentifier }?.profile
            guard profile != self.model.activeAppProfile else { return }
            self.model.activeAppProfile = profile
            self.server.settingsChanged()
        }
    }

    // ⌘⌥1 / ⌘⌥2 / ⌘⌥3: the one-click layouts without opening the panel. ⌘⌥N: the panel.
    private func registerLayoutHotKeys() {
        hotKeys.register(keyCode: kVK_ANSI_N, modifiers: cmdKey | optionKey) { [weak self] in self?.togglePanel() }
        // ⌘⌥[ / ⌘⌥]: the glasses' brightness.
        for (key, step) in [(kVK_ANSI_LeftBracket, -1), (kVK_ANSI_RightBracket, 1)] {
            hotKeys.register(keyCode: key, modifiers: cmdKey | optionKey) { [weak self] in
                guard let self else { return }
                let current = self.model.brightness == 0 ? 8 : self.model.brightness
                self.model.brightness = max(1, min(8, current + step))
                self.server.settingsChanged()
            }
        }
        let keys = [kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3]
        for (key, preset) in zip(keys, AppModel.layouts) {
            hotKeys.register(keyCode: key, modifiers: cmdKey | optionKey) { [weak self] in
                guard let self, self.model.activeLayout != preset.id else { return }
                self.model.apply(preset)
                self.server.settingsChanged()
                self.server.announce(preset.title)
            }
        }
    }

    // Compact glass panel under the menu bar. A window of its own rather than an NSPopover:
    // when the ◎ item is hidden behind the notch (a crowded menu bar), a popover anchored to
    // it lands half off the top of the screen.
    @objc func togglePanel() {
        if let panel, panel.isVisible { panel.orderOut(nil); return }
        // The click on ◎ that just closed it (focus moved to the menu bar): stay closed.
        if let panel, Date().timeIntervalSince(panel.closedAt) < 0.3 { return }
        showPanel()
    }

    func showPanel() {
        if panel == nil {
            let host = NSHostingView(rootView: QuickPanelContainer(layout: panelLayout, model: model, server: server) { [weak self] in
                self?.panel?.orderOut(nil)
                self?.openLayout()
            }
            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous)))
            let p = KeyPanel(contentRect: NSRect(origin: .zero, size: host.fittingSize),
                             styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
            p.contentView = host
            p.isOpaque = false
            p.backgroundColor = .clear
            p.hasShadow = true
            p.level = .popUpMenu
            p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
            p.appearance = NSAppearance(named: .darkAqua)
            p.hidesOnDeactivate = false
            p.onResignKey = { [weak p] in p?.orderOut(nil) }  // click elsewhere: close, like a popover
            // Dragged like a window (the top bar, or any empty part of it); it opens there again.
            p.isMovable = true
            p.isMovableByWindowBackground = true
            NotificationCenter.default.addObserver(forName: NSWindow.didMoveNotification, object: p, queue: .main) { [weak self, weak p] _ in
                guard let self, let p, !self.panelPlacing else { return }
                UserDefaults.standard.set([p.frame.minX, p.frame.maxY], forKey: Self.panelTopLeftKey)
            }
            panel = p
        }
        guard let panel, let host = panel.contentView else { return }
        // Where it was dragged to last, else under the ◎ item when it is on screen, else at the
        // right edge. Always fully inside the visible area; taller than it, it scrolls, so
        // "Más opciones" can always be reached.
        let item = statusItem.button?.window
        let saved = (UserDefaults.standard.array(forKey: Self.panelTopLeftKey) as? [Double]).flatMap {
            $0.count == 2 ? NSPoint(x: $0[0], y: $0[1]) : nil
        }
        let screen = saved.flatMap { p in NSScreen.screens.first { $0.frame.insetBy(dx: -1, dy: -1).contains(p) } }
            ?? item?.screen ?? NSScreen.main ?? NSScreen.screens[0]
        let visible = screen.visibleFrame
        panelLayout.maxHeight = nil
        host.layoutSubtreeIfNeeded()
        var size = host.fittingSize
        if size.height > visible.height - 12 {
            size.height = visible.height - 12
            panelLayout.maxHeight = size.height
        }
        var x = visible.maxX - size.width - 10
        var top = visible.maxY - 6
        if let saved {
            x = saved.x
            top = saved.y
        } else if let item, item.occlusionState.contains(.visible), screen.frame.contains(item.frame) {
            x = item.frame.midX - size.width / 2
        }
        x = max(visible.minX + 8, min(x, visible.maxX - size.width - 8))
        top = max(visible.minY + size.height + 6, min(top, visible.maxY - 6))
        panelPlacing = true
        panel.setFrame(NSRect(x: x, y: top - size.height, width: size.width, height: size.height), display: true)
        panelPlacing = false
        // Key without activating the app: Nimbo hides the hardware cursor, and as the active
        // app that hiding also took the cursor out of the captured panels until another app
        // was clicked.
        panel.makeKeyAndOrderFront(nil)
    }

    private var previewWindow: NSWindow?

    private func showPanelPreviewWindow() {
        let host = NSHostingView(rootView: QuickPanel(model: model, server: server) {})
        let w = NSWindow(contentRect: NSRect(origin: .zero, size: host.fittingSize), styleMask: [.titled, .fullSizeContentView],
                         backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.isOpaque = false
        w.backgroundColor = .clear
        w.contentView = host
        w.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        w.level = .floating
        w.center()
        w.orderFrontRegardless()
        previewWindow = w
    }

    @objc func openLayout() {
        if window == nil {
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 880, height: 900),
                             styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                             backing: .buffered, defer: false)
            w.title = "Nimbo"
            w.titlebarAppearsTransparent = true
            w.isReleasedWhenClosed = false
            w.backgroundColor = NSColor(Theme.background)
            w.contentView = NSHostingView(rootView: MainView(model: model, server: server))
            w.minSize = NSSize(width: 600, height: 380)
            w.center()
            window = w
        }
        // Fully on the screen it opens on, title bar included (a small virtual monitor is
        // shorter than the window: its close button ended up off screen).
        if let w = window, let room = (w.screen ?? NSScreen.main)?.visibleFrame {
            var f = w.frame
            f.size.width = min(f.width, room.width - 20)
            f.size.height = min(f.height, room.height - 20)
            f.origin.x = max(room.minX + 10, min(f.minX, room.maxX - f.width - 10))
            f.origin.y = max(room.minY + 10, min(f.minY, room.maxY - f.height - 10))
            if !room.intersects(w.frame) { f.origin = CGPoint(x: room.midX - f.width / 2, y: room.midY - f.height / 2) }
            w.setFrame(f, display: false)
        }
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        if window?.title == "Nimbo" { window?.title = "Nimbo · Más opciones" }
    }

    // Click: the panel. Right-click: the quick menu.
    private func buildStatusItem() {
        guard let button = statusItem.button else { return }
        // Template image: macOS tints it for light/dark menu bars and the highlighted state.
        if let icon = Bundle.main.image(forResource: "MenuBarIcon") {
            icon.isTemplate = true
            icon.size = NSSize(width: 18, height: 18)
            button.image = icon
        } else {
            button.title = "◎"
        }
        button.target = self
        button.action = #selector(statusClicked)
        button.sendAction(on: [.leftMouseUp, .rightMouseUp])
    }

    @objc private func statusClicked() {
        if NSApp.currentEvent?.type == .rightMouseUp {
            let menu = NSMenu()
            let items: [(String, Selector)] = [("Recentrar (⌘⌥C)", #selector(recenter)),
                                               (server.isRunning ? "Parar" : "Empezar", #selector(toggleServer)),
                                               ("Abrir disposición", #selector(openLayout))]
            for (title, sel) in items {
                let it = NSMenuItem(title: title, action: sel, keyEquivalent: "")
                it.target = self
                menu.addItem(it)
            }
            menu.addItem(.separator())
            let quit = NSMenuItem(title: "Salir", action: #selector(quit), keyEquivalent: "")
            quit.target = self
            menu.addItem(quit)
            statusItem.menu = menu
            statusItem.button?.performClick(nil)
            statusItem.menu = nil
        } else {
            togglePanel()
        }
    }

    @objc private func recenter() { server.recenter() }
    @objc private func toggleServer() { server.toggle() }
    @objc private func quit() { NSApp.terminate(nil) }
}

// Borderless panel that can take key focus (sliders, toggles) and closes when it loses it.
// The quick panel's height limit when the screen is shorter than it (nil: its natural height).
final class PanelLayout: ObservableObject {
    @Published var maxHeight: CGFloat?
}

@available(macOS 14.0, *)
struct QuickPanelContainer: View {
    @ObservedObject var layout: PanelLayout
    @ObservedObject var model: AppModel
    @ObservedObject var server: ServerController
    let openMore: () -> Void

    var body: some View {
        if let h = layout.maxHeight {
            ScrollView(.vertical, showsIndicators: true) {
                QuickPanel(model: model, server: server, openMore: openMore)
            }
            .frame(width: 380, height: h)
        } else {
            QuickPanel(model: model, server: server, openMore: openMore)
        }
    }
}

final class KeyPanel: NSPanel {
    var onResignKey: (() -> Void)?
    private(set) var closedAt = Date.distantPast
    override var canBecomeKey: Bool { true }
    override func resignKey() {
        super.resignKey()
        onResignKey?()
    }
    override func orderOut(_ sender: Any?) {
        if isVisible { closedAt = Date() }
        super.orderOut(sender)
    }
}
