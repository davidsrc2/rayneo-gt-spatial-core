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
                    Toggle("Mis gafas están en modo pinned", isOn: $model.glassesPinned)
                    Text("El modo se cambia con el botón de las gafas. Con esto activado, ⌘⌥C recentra también las gafas.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                }
                .font(Theme.body).foregroundStyle(Theme.text)
                VStack(alignment: .leading, spacing: 6) {
                    Picker("Las pantallas siguen", selection: $model.tracking) {
                        Text("Solo horizontal").tag(1)
                        Text("Solo vertical").tag(2)
                        Text("Ambos").tag(0)
                    }
                    .pickerStyle(.segmented)
                    Text("Horizontal: monitores en fila, quietos al mirar arriba o abajo. Vertical: monitores uno encima de otro.")
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
                    LabeledSlider(title: "Zona quieta", value: $model.deadZone, range: 0.3...2.5, step: 0.1,
                                  caption: String(format: "%.1f°", model.deadZone))
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

            Spacer()
            HStack {
                Spacer()
                Button("Hecho") { dismiss() }.buttonStyle(CalmButtonStyle(prominent: true))
            }
        }
        .padding(28)
        .frame(width: 640, height: advanced ? 800 : 440)
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
        .onChange(of: model.k1) { server.settingsChanged() }
        .onChange(of: model.k2) { server.settingsChanged() }
    }
}
