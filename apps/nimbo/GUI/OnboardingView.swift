import ApplicationServices
import Combine
import SwiftUI

@available(macOS 14.0, *)
struct OnboardingView: View {
    enum Step: Int { case welcome, calibration, scale }

    @ObservedObject var model: AppModel
    @ObservedObject var server: ServerController
    @StateObject private var stepBox: Box<Step>
    let onDone: () -> Void
    private var step: Step { stepBox.value }

    init(model: AppModel, server: ServerController, startAt: Step, onDone: @escaping () -> Void) {
        self.model = model
        self.server = server
        _stepBox = StateObject(wrappedValue: Box(startAt))
        self.onDone = onDone
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            HStack(spacing: 8) {
                ForEach(0..<3) { i in
                    Capsule().fill(i <= step.rawValue ? Theme.accent : Theme.line).frame(width: 36, height: 4)
                }
            }
            Group {
                switch step {
                case .welcome: PermissionsStep { go(.calibration) }
                case .calibration: CalibrationStep(server: server, onNext: { go(.scale) }, onSkip: { go(.scale) })
                case .scale: ScaleStep(model: model, server: server, onDone: onDone)
                }
            }
            .transition(.opacity)
        }
        .padding(32)
        .frame(width: 640, height: 560, alignment: .topLeading)
        .background(Theme.background)
        .preferredColorScheme(.dark)
        .animation(Theme.ease, value: step)
    }

    private func go(_ s: Step) { withAnimation(Theme.ease) { stepBox.value = s } }
}

// MARK: permissions

private struct PermissionsStep: View {
    let onNext: () -> Void
    @StateObject private var screenBox = Box(CGPreflightScreenCaptureAccess())
    @StateObject private var accessibilityBox = Box(AXIsProcessTrusted())
    private var screen: Bool { screenBox.value }
    private var accessibility: Bool { accessibilityBox.value }
    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Bienvenido").font(Theme.title).foregroundStyle(Theme.text)
            Text("Para mostrar tus monitores en las gafas, el Mac necesita dos permisos. Solo se piden una vez.")
                .font(Theme.body).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
            permission(title: "Grabación de pantalla",
                       detail: "Para ver el contenido de los monitores dentro de las gafas.",
                       granted: screen,
                       pane: "Privacy_ScreenCapture") { _ = CGRequestScreenCaptureAccess() }
            permission(title: "Accesibilidad",
                       detail: "Para traer tus ventanas abiertas a los monitores al empezar.",
                       granted: accessibility,
                       pane: "Privacy_Accessibility") {
                let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
                _ = AXIsProcessTrustedWithOptions([key: true] as CFDictionary)
            }
            Text("Si acabas de dar el permiso de grabación, puede que haya que abrir la app de nuevo.")
                .font(Theme.small).foregroundStyle(Theme.secondary)
            Spacer()
            HStack {
                Spacer()
                Button("Continuar", action: onNext).buttonStyle(CalmButtonStyle(prominent: true))
            }
        }
        .onReceive(timer) { _ in
            screenBox.value = CGPreflightScreenCaptureAccess()
            accessibilityBox.value = AXIsProcessTrusted()
        }
    }

    private func permission(title: String, detail: String, granted: Bool, pane: String,
                            request: @escaping () -> Void) -> some View {
        HStack(spacing: 16) {
            Image(systemName: granted ? "checkmark.circle" : "circle")
                .font(.system(size: 22, weight: .light))
                .foregroundStyle(granted ? Theme.accent : Theme.secondary)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(Theme.body).foregroundStyle(Theme.text)
                Text(detail).font(Theme.small).foregroundStyle(Theme.secondary)
            }
            Spacer()
            if !granted {
                Button("Abrir Ajustes") {
                    request()
                    if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") {
                        NSWorkspace.shared.open(url)
                    }
                }
                .buttonStyle(CalmButtonStyle())
            }
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Theme.surface))
        .animation(Theme.ease, value: granted)
    }
}

// MARK: calibration

@available(macOS 14.0, *)
private struct CalibrationStep: View {
    @ObservedObject var server: ServerController
    let onNext: () -> Void
    let onSkip: () -> Void

    @StateObject private var calibratorBox = Box(RNCalibrator())
    @StateObject private var snapBox = Box<RNCalibrationSnapshot?>(nil)
    @StateObject private var startedBox = Box(false)
    private var calibrator: RNCalibrator { calibratorBox.value }
    private var snap: RNCalibrationSnapshot? { snapBox.value }
    private var started: Bool { startedBox.value }
    private let timer = Timer.publish(every: 0.1, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Calibrar las gafas").font(Theme.title).foregroundStyle(Theme.text)
            Text("Así las gafas saben hacia dónde miras. Tarda menos de un minuto.")
                .font(Theme.body).foregroundStyle(Theme.secondary)

            HStack(spacing: 28) {
                Image(systemName: symbol)
                    .font(.system(size: 64, weight: .ultraLight))
                    .foregroundStyle(Theme.accent)
                    .frame(width: 110, height: 110)
                    .contentTransition(.opacity)
                VStack(alignment: .leading, spacing: 8) {
                    Text(instruction).font(.system(size: 19)).foregroundStyle(Theme.text)
                        .fixedSize(horizontal: false, vertical: true)
                    Text(hint).font(Theme.label).foregroundStyle(Theme.secondary)
                }
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Theme.surface))
            .animation(Theme.ease, value: instruction)

            ProgressView(value: progress).tint(Theme.accent).animation(Theme.ease, value: progress)

            if let snap, snap.finished {
                Text(snap.succeeded ? "Listo. Calibración guardada." : "No ha salido bien: \(snap.message). Puedes repetirla.")
                    .font(Theme.body).foregroundStyle(snap.succeeded ? Theme.accent : Theme.secondary)
            }
            Spacer()
            HStack(spacing: 12) {
                Button("Saltar") { calibrator.cancel(); onSkip() }.buttonStyle(CalmButtonStyle())
                Spacer()
                if snap?.finished == true && snap?.succeeded == true {
                    Button("Continuar", action: onNext).buttonStyle(CalmButtonStyle(prominent: true))
                } else if snap?.running == true {
                    Button("Cancelar") { calibrator.cancel() }.buttonStyle(CalmButtonStyle())
                } else {
                    Button(started ? "Repetir" : "Empezar calibración") {
                        server.stop()  // the glasses can only be read by one at a time
                        startedBox.value = true
                        calibrator.startWriting(to: AppModel.calibrationURL.path)
                    }
                    .buttonStyle(CalmButtonStyle(prominent: true))
                }
            }
        }
        .onReceive(timer) { _ in if started { snapBox.value = calibrator.snapshot() } }
        .onDisappear { calibrator.cancel() }
    }

    private var stepIndex: Int {
        guard let snap, snap.running || snap.finished else { return 0 }
        return snap.step.rawValue
    }

    private var progress: Double {
        guard let snap else { return 0 }
        if snap.finished && snap.succeeded { return 1 }
        return (Double(snap.step.rawValue) + (snap.returning ? 0.5 : 0)) / 6.0
    }

    private var symbol: String {
        guard started, let snap, snap.running else { return "eyeglasses" }
        if snap.returning { return "circle.dotted" }
        switch snap.step {
        case .bias: return "tray"
        case .center: return "eyeglasses"
        case .up: return "arrow.up.circle"
        case .down: return "arrow.down.circle"
        case .left: return "arrow.left.circle"
        case .right: return "arrow.right.circle"
        @unknown default: return "eyeglasses"
        }
    }

    private var instruction: String {
        guard started, let snap, snap.running else {
            return "Conecta las gafas y déjalas quietas sobre la mesa. Luego pulsa «Empezar calibración»."
        }
        if snap.returning { return "Bien. Vuelve a mirar al frente y quédate quieto." }
        switch snap.step {
        case .bias: return "Deja las gafas quietas sobre la mesa unos segundos."
        case .center: return "Ponte las gafas y mira al frente, con la cabeza recta."
        case .up: return "Mira hacia arriba y quédate así."
        case .down: return "Mira hacia abajo y quédate así."
        case .left: return "Gira la cabeza a la izquierda y quédate así."
        case .right: return "Gira la cabeza a la derecha y quédate así."
        @unknown default: return ""
        }
    }

    private var hint: String {
        guard started, let snap, snap.running else { return "" }
        switch snap.hint {
        case "further": return "Un poco más…"
        case "hold": return "Así, quieto…"
        case "move": return snap.returning ? "" : "Despacio, unos 30 grados"
        case "waiting for rest": return "Esperando a que estén quietas…"
        case "put the glasses on": return "Cuando las tengas puestas, quédate quieto"
        default: return ""
        }
    }
}

// MARK: scale

@available(macOS 14.0, *)
private struct ScaleStep: View {
    @ObservedObject var model: AppModel
    @ObservedObject var server: ServerController
    let onDone: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Ajustar la escala").font(Theme.title).foregroundStyle(Theme.text)
            Text("Con las gafas puestas, gira la cabeza despacio a izquierda y derecha mirando el borde de un monitor.")
                .font(Theme.body).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 10) {
                Label("Si los monitores se mueven contigo, pulsa −", systemImage: "arrow.left.and.right")
                Label("Si se mueven en sentido contrario, pulsa +", systemImage: "arrow.uturn.left")
                Label("Cuando se queden quietos, ya está", systemImage: "checkmark")
            }
            .font(Theme.body).foregroundStyle(Theme.text)

            HStack(spacing: 18) {
                Button { change(-0.5) } label: { Image(systemName: "minus").frame(width: 30) }
                    .buttonStyle(CalmButtonStyle())
                Text(String(format: "%.1f", model.fov)).font(.system(size: 22, weight: .light)).foregroundStyle(Theme.text)
                    .frame(width: 70)
                Button { change(0.5) } label: { Image(systemName: "plus").frame(width: 30) }
                    .buttonStyle(CalmButtonStyle())
                Text("También con ⌘⌥− y ⌘⌥=").font(Theme.small).foregroundStyle(Theme.secondary)
            }
            Spacer()
            HStack {
                if !server.isRunning {
                    Button("Ver los monitores") { server.start() }.buttonStyle(CalmButtonStyle())
                }
                Spacer()
                Button("Terminar", action: onDone).buttonStyle(CalmButtonStyle(prominent: true))
            }
        }
    }

    private func change(_ delta: Double) {
        model.fov = max(15, min(35, model.fov + delta))
        server.settingsChanged()
    }
}
