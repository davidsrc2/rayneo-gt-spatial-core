import SwiftUI

@available(macOS 14.0, *)
struct SettingsView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var server: ServerController
    let onRecalibrate: () -> Void
    @Environment(\.dismiss) private var dismiss
    @StateObject private var advancedBox = Box(false)
    private var advanced: Bool { advancedBox.value }

    var body: some View {
        // Scrolls, with "Hecho" always in view (also Esc), and never taller than the screen it
        // opens on: a fixed height cut the button off and the sheet could not be closed.
        VStack(spacing: 0) {
            ScrollView { content.padding(28) }
            Divider().opacity(0.4)
            HStack {
                Spacer()
                Button("Hecho") { dismiss() }
                    .buttonStyle(CalmButtonStyle(prominent: true))
                    .keyboardShortcut(.cancelAction)
            }
            .padding(.horizontal, 28)
            .padding(.vertical, 14)
        }
        .frame(width: 640, height: min(advanced ? 800 : 620, Self.screenRoom))
        .background(Theme.background)
        .preferredColorScheme(.dark)
        .animation(Theme.ease, value: advanced)
        .onChange(of: model.fov) { server.settingsChanged() }
        .onChange(of: model.ipd) { server.settingsChanged() }
        .onChange(of: model.deadZone) { server.settingsChanged() }
        .onChange(of: model.stereo) { server.settingsChanged() }
        .onChange(of: model.gazeCursor) { server.settingsChanged() }
        .onChange(of: model.dimOthers) { server.settingsChanged() }
        .onChange(of: model.tracking) { server.settingsChanged() }
        .onChange(of: model.focusWindows) { server.settingsChanged() }
        .onChange(of: model.macScreen) { server.settingsChanged() }
        .onChange(of: model.k1) { server.settingsChanged() }
        .onChange(of: model.k2) { server.settingsChanged() }
    }

    // Height the sheet may take: the screen with the key window, less room for its title bar.
    private static var screenRoom: CGFloat {
        let screen = NSApp.keyWindow?.screen ?? NSScreen.main
        return max(320, (screen?.visibleFrame.height ?? 900) - 90)
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("Ajustes").font(Theme.title).foregroundStyle(Theme.text)

            Section(title: "Calibración") {
                Text(model.hasCalibration ? "Tus gafas están calibradas." : "Aún no has calibrado tus gafas.")
                    .font(Theme.body).foregroundStyle(Theme.text)
                Button("Volver a calibrar", action: onRecalibrate).buttonStyle(CalmButtonStyle())
            }

            Section(title: "Pantallas") {
                Toggle("Atenuar los monitores que no miras", isOn: $model.dimOthers)
                    .font(Theme.body).foregroundStyle(Theme.text)
                Text("Cada pantalla mide lo que la imagen de las gafas y queda quieta en la habitación: gira la cabeza para pasar de una a otra. Nimbo pone las gafas en follow y, al parar, las devuelve a pinned. ⌘⌥C las vuelve a poner delante.")
                    .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                VStack(alignment: .leading, spacing: 6) {
                    Toggle("La pantalla del Mac es una de las de Nimbo", isOn: $model.macScreen)
                    Text("Con la tapa abierta, la pantalla del Mac es la pantalla 1 y las demás son virtuales a su lado: con 2 pantallas, la del Mac y una más; con 1, solo la del Mac.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                }
                .font(Theme.body).foregroundStyle(Theme.text)
                VStack(alignment: .leading, spacing: 6) {
                    Toggle("Incluir ventanas flotantes", isOn: $model.focusWindows)
                    Text("Si una ventana flotante puede ser la pantalla del modo enfoque (⌘⌥O) y del modo cine (⌘⌥P). Desactivado: solo los monitores.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                }
                .font(Theme.body).foregroundStyle(Theme.text)
            }

            DisclosureGroup(isExpanded: $advancedBox.value) {
                VStack(alignment: .leading, spacing: 14) {
                    LabeledSlider(title: "Campo de visión", value: $model.fov, range: 15...35, step: 0.5,
                                  caption: String(format: "%.1f°", model.fov))
                    LabeledSlider(title: "Distancia entre ojos", value: $model.ipd, range: 54...72, step: 1,
                                  caption: String(format: "%.0f mm", model.ipd))
                    if model.glassesPinned {  // space at real size: never frozen, always fixed in the room
                        LabeledSlider(title: "Zona quieta", value: $model.deadZone, range: 0.3...2.5, step: 0.1,
                                      caption: String(format: "%.1f°", model.deadZone))
                    }
                    Toggle("Imagen en 3D (gafas en modo 3D)", isOn: $model.stereo)
                    Toggle("El cursor sigue a tu mirada", isOn: $model.gazeCursor)
                    Toggle("Traer mis ventanas al empezar", isOn: $model.gatherWindows)
                    LabeledSlider(title: "Corrección de lente k1", value: $model.k1, range: -0.2...0.2, step: 0.01,
                                  caption: String(format: "%.2f", model.k1))
                    LabeledSlider(title: "Corrección de lente k2", value: $model.k2, range: -0.2...0.2, step: 0.01,
                                  caption: String(format: "%.2f", model.k2))
                }
                .font(Theme.body)
                .foregroundStyle(Theme.text)
                .padding(.top, 12)
            } label: {
                Text("Avanzado").font(Theme.body).foregroundStyle(Theme.secondary)
            }
            .tint(Theme.secondary)

        }
    }
}
