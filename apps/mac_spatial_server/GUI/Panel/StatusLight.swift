import SwiftUI

// Traffic light: green = sensors stable, amber = calibrating / starting, grey = off.
@available(macOS 14.0, *)
struct StatusLight: View {
    @ObservedObject var server: ServerController

    private var color: Color {
        switch server.state {
        case .tracking: return Color(red: 0.48, green: 0.78, blue: 0.60)
        case .calibrating, .starting: return Color(red: 0.89, green: 0.71, blue: 0.40)
        case .stopped: return Color(white: 0.55)
        }
    }

    private var text: String {
        switch server.state {
        case .tracking: return "Sensores estabilizados"
        case .calibrating: return "Calibrando (deja las gafas en la mesa 5 s)"
        case .starting: return "Preparando los monitores…"
        case .stopped: return server.glassesConnected ? "Listas para empezar" : "Conecta las RayNeo GT"
        }
    }

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 9, height: 9)
                .shadow(color: color.opacity(0.7), radius: server.state == .stopped ? 0 : 4)
            Text(text).font(.system(size: 13)).foregroundStyle(.secondary).lineLimit(1)
        }
        .animation(.easeInOut(duration: 0.4), value: server.state)
    }
}
