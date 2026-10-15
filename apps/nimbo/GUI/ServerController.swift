import AppKit
import Combine

// Runs SpatialServer inside the app and turns its state into calm status text.
@available(macOS 14.0, *)
final class ServerController: ObservableObject {
    enum State: Equatable {
        case stopped, starting, calibrating, tracking
    }

    @Published private(set) var state: State = .stopped
    @Published private(set) var glassesConnected = Displays.glassesScreen() != nil
    @Published var notice: String?  // transient, shown softly under the status line
    @Published private(set) var gazePanel: Int?  // monitor you are looking at, for the panel

    private var server: SpatialServer?
    private var pending: (server: SpatialServer, task: Task<Void, Never>)?  // while starting
    private var poll: Timer?
    private var gazePoll: Timer?
    private weak var model: AppModel?
    private var runningOptions: ServerOptions?
    private var autoStartChecked = false
    private let notifier = Notifier()
    private var glassesPresent = Displays.glassesScreen() != nil  // the display itself, for notices

    var isRunning: Bool { state != .stopped }

    var statusText: String {
        switch state {
        case .starting: return "Preparando los monitores…"
        case .calibrating: return "Calibrando, deja las gafas quietas sobre la mesa…"
        case .tracking: return "Gafas conectadas · siguiendo tu cabeza"
        case .stopped: return glassesConnected ? "Gafas conectadas · listas para empezar" : "Gafas desconectadas"
        }
    }

    init() {
        poll = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.refresh() }
        gazePoll = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self else { return }
            let g = self.server?.gazePanel()
            if g != self.gazePanel { self.gazePanel = g }  // publish changes only
        }
    }

    func bind(_ model: AppModel) { self.model = model }

    func start() {
        guard let model, server == nil, pending == nil else { return }
        let options = model.serverOptions()
        guard options.windowed || Displays.glassesScreen() != nil else {
            say("Conecta las gafas para empezar")
            return
        }
        state = .starting
        let s = SpatialServer(options: options)
        s.onStopRequest = { [weak self] reason in
            self?.stop(notice: reason.contains("disconnected") || reason.contains("IMU")
                                ? "Las gafas se han desconectado; todo ha vuelto a la normalidad" : nil)
        }
        s.onFovChanged = { [weak self] fov in self?.model?.fov = Double(fov) }
        s.onNotice = { [weak self] text in self?.notifier.post("Nimbo", text) }
        // Setup takes a few seconds (display modes, virtual monitors); the panel stays live.
        let task = Task { @MainActor [weak self] in
            do {
                try await s.start()
                guard let self, self.pending?.server === s else { return s.stop() }
                self.pending = nil
                self.server = s
                self.runningOptions = options
                self.runningOptions?.floatingWindows = s.floatingResolved
                self.model?.floating = s.floatingResolved  // poses chosen by the server
                self.state = .calibrating
            } catch {
                s.stop()
                guard let self, self.pending?.server === s else { return }
                self.pending = nil
                self.state = .stopped
                if !(error is CancellationError) { self.say("No se pudo empezar: \(error.localizedDescription)") }
            }
        }
        pending = (s, task)
    }

    func stop(notice: String? = nil) {
        // Still starting: cancel, and the start task tears down what it built so far.
        pending?.task.cancel()
        pending = nil
        server?.stop()
        server = nil
        runningOptions = nil
        state = .stopped
        if let notice { say(notice) }
    }

    func toggle() { isRunning ? stop() : start() }

    func recenter() { server?.recenter() }

    // Live where possible; monitors, text size or the stabilizer need a restart.
    func settingsChanged() {
        guard let model, let server, let running = runningOptions else { return }
        let next = model.serverOptions()
        let needsRestart = next.layout != running.layout || next.vdWidth != running.vdWidth ||
            next.deadZoneDeg != running.deadZoneDeg
        if needsRestart {
            stop()
            start()
        } else {
            server.applyLive(next)
            let ids = { (o: ServerOptions) in o.floatingWindows.map(\.windowID) }
            if ids(next) != ids(running) {
                // Windows added or removed: their captures restart, then the panel gets the
                // poses the server chose (centred above the monitors).
                Task { @MainActor [weak self, weak server] in
                    guard let server else { return }
                    await server.setFloatingWindows(next.floatingWindows)
                    self?.runningOptions?.floatingWindows = server.floatingResolved
                    self?.model?.floating = server.floatingResolved
                }
            } else {
                server.applyFloatingPoses(next.floatingWindows.map { $0.pose ?? PanelPos(yaw: 0, pitch: 0) })
            }
            runningOptions = next
        }
    }

    // A short notification (shortcuts change things with no panel on screen).
    func tell(_ title: String, _ body: String = "") { notifier.post(title, body) }

    // Layout switched by shortcut: no panel on screen, so tell it with a notice.
    func announce(_ layout: String) {
        notifier.post(layout, isRunning ? "Preparando los monitores…" : "Se aplicará al empezar")
    }

    private func refresh() {
        // While running or starting, display reconfigurations can hide the glasses briefly.
        let present = Displays.glassesScreen() != nil || server != nil || pending != nil
        if present != glassesPresent {
            glassesPresent = present
            if present {
                notifier.post("Gafas conectadas", model?.autoStart == true ? "Empezando…" : "Abre Nimbo para empezar")
            } else {
                notifier.post("Gafas desconectadas", "Tus pantallas han vuelto a la normalidad")
            }
        }
        let was = glassesConnected
        glassesConnected = Displays.glassesScreen() != nil || server != nil
        // Plug and forget: start when the glasses appear (or are already there at launch).
        if let model, model.autoStart, server == nil, pending == nil, state == .stopped, glassesConnected,
           !was || !autoStartChecked {
            start()
        }
        autoStartChecked = true
        guard let server else { return }
        state = server.isTracking ? .tracking : .calibrating
    }

    private func say(_ text: String) {
        notice = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) { [weak self] in
            if self?.notice == text { self?.notice = nil }
        }
    }
}
