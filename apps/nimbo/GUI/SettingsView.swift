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
        .onChange(of: model.glassesPinned) { server.settingsChanged() }
        .onChange(of: model.growFocused) { server.settingsChanged() }
        .onChange(of: model.focusWindows) { server.settingsChanged() }
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
                VStack(alignment: .leading, spacing: 6) {
                    Picker("Cómo ves las pantallas", selection: $model.glassesPinned) {
                        Text("Una pantalla grande").tag(true)
                        Text("Espacio a tamaño real").tag(false)
                    }
                    .pickerStyle(.segmented)
                    Text(model.glassesPinned
                         ? "Las gafas en pinned (Nimbo las pone): su imagen queda fija y todo cabe dentro, nada se mueve solo. ⌘⌥C vuelve a centrar la imagen delante."
                         : "Las gafas en follow (Nimbo las pone; al parar vuelven a pinned): cada pantalla del tamaño de la imagen de las gafas, como sin Nimbo, quietas en la habitación. Gira la cabeza para ver la de al lado o mira arriba para la ventana de arriba. ⌘⌥C las vuelve a poner delante.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                    if model.glassesPinned {
                        Toggle("Agrandar la pantalla que miras", isOn: $model.growFocused)
                        Text("La pantalla que miras un segundo, o a la que llevas el ratón, ocupa toda la imagen de las gafas, grande y nítida como sin Nimbo. Las demás quedan justo fuera de los bordes: mira hacia ellas un segundo para pasar a otra. Desactivado: se ve todo junto, más pequeño.")
                            .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                    }
                }
                .font(Theme.body).foregroundStyle(Theme.text)
                VStack(alignment: .leading, spacing: 6) {
                    Toggle("Incluir ventanas flotantes", isOn: $model.focusWindows)
                    Text("Si una ventana flotante puede ser la pantalla agrandada en pinned o la del modo enfoque (⌘⌥O) y cine (⌘⌥P). Desactivado: solo los monitores.")
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
