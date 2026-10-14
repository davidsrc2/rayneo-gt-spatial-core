import AppKit

// Menu bar item: recenter / quit without remembering the hotkeys.
final class StatusMenu: NSObject {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let onRecenter: () -> Void

    init(onRecenter: @escaping () -> Void) {
        self.onRecenter = onRecenter
        super.init()
        item.button?.title = "◎ Nimbo"
        let menu = NSMenu()
        let recenter = NSMenuItem(title: "Recentrar (⌘⌥C)", action: #selector(recenterAction), keyEquivalent: "")
        recenter.target = self
        menu.addItem(recenter)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Salir (⌘⌥Q)", action: #selector(quitAction), keyEquivalent: "")
        quit.target = self
        menu.addItem(quit)
        item.menu = menu
    }

    deinit { NSStatusBar.system.removeStatusItem(item) }

    @objc private func recenterAction() { onRecenter() }
    @objc private func quitAction() { NSApp.terminate(nil) }
}
