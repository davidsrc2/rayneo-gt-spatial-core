import AppKit
import SwiftUI

// The app when opened normally (no command-line flags): one calm window plus "◎" in the
// menu bar. The server runs in-process and is started/stopped from either.
@available(macOS 14.0, *)
final class GUIApp: NSObject, NSMenuDelegate {
    let model = AppModel()
    let server = ServerController()
    private var window: NSWindow?
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let toggleItem = NSMenuItem(title: "Empezar", action: #selector(toggleServer), keyEquivalent: "")

    func launch() {
        NSApp.setActivationPolicy(.regular)
        NSApp.appearance = NSAppearance(named: .darkAqua)
        server.bind(model)
        buildStatusMenu()
        openLayout()
    }

    func shutdown() { server.stop() }

    @objc func openLayout() {
        if window == nil {
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 880, height: 900),
                             styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                             backing: .buffered, defer: false)
            w.title = "RayNeo"
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
    }

    private func buildStatusMenu() {
        statusItem.button?.title = "◎"
        statusItem.button?.font = NSFont.systemFont(ofSize: 15)
        let menu = NSMenu()
        menu.delegate = self
        let recenter = NSMenuItem(title: "Recentrar (⌘⌥C)", action: #selector(recenter), keyEquivalent: "")
        recenter.target = self
        toggleItem.target = self
        let layout = NSMenuItem(title: "Abrir disposición", action: #selector(openLayout), keyEquivalent: "")
        layout.target = self
        let quit = NSMenuItem(title: "Salir", action: #selector(quit), keyEquivalent: "")
        quit.target = self
        [recenter, toggleItem, layout].forEach(menu.addItem)
        menu.addItem(.separator())
        menu.addItem(quit)
        statusItem.menu = menu
    }

    func menuWillOpen(_ menu: NSMenu) {
        toggleItem.title = server.isRunning ? "Parar" : "Empezar"
    }

    @objc private func recenter() { server.recenter() }
    @objc private func toggleServer() { server.toggle() }
    @objc private func quit() { NSApp.terminate(nil) }
}
