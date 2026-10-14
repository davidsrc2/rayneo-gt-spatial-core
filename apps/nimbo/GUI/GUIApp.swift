import AppKit
import SwiftUI

// The app when opened normally (no command-line flags): one calm window plus "◎" in the
// menu bar. The server runs in-process and is started/stopped from either.
@available(macOS 14.0, *)
final class GUIApp: NSObject {
    let model = AppModel()
    let server = ServerController()
    private var window: NSWindow?
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let popover = NSPopover()

    func launch() {
        NSApp.setActivationPolicy(.accessory)  // a menu bar app: no Dock icon
        NSApp.appearance = NSAppearance(named: .darkAqua)
        server.bind(model)
        buildStatusItem()
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

    // Compact glass panel under the ◎ icon.
    @objc func togglePanel() {
        popover.isShown ? popover.performClose(nil) : showPanel()
    }

    func showPanel() {
        guard let button = statusItem.button else { return }
        if popover.contentViewController == nil {
            popover.behavior = .transient
            popover.animates = true
            popover.appearance = NSAppearance(named: .darkAqua)
            popover.contentViewController = NSHostingController(rootView: QuickPanel(model: model, server: server) { [weak self] in
                self?.popover.performClose(nil)
                self?.openLayout()
            })
        }
        // A transient popover of an inactive app closes at once: become active first.
        NSApp.activate(ignoringOtherApps: true)
        popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        popover.contentViewController?.view.window?.makeKey()
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
            w.minSize = NSSize(width: 820, height: 640)
            w.center()
            window = w
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
