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
        .onChange(of: model.gazeCursor) { server.settingsChanged() }
        .onChange(of: model.dimOthers) { server.settingsChanged() }
        .onChange(of: model.tracking) { server.settingsChanged() }
        .onChange(of: model.focusWindows) { server.settingsChanged() }
        .onChange(of: model.macScreen) { server.settingsChanged() }
        .onChange(of: model.anchorAtRest) { server.settingsChanged() }
        .onChange(of: model.depthStrength) { server.settingsChanged() }
        .onChange(of: model.depthPlane) { server.settingsChanged() }
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
                    Toggle("Anclar las pantallas con la cabeza quieta", isOn: $model.anchorAtRest)
                    Text("Con la cabeza quieta, las pantallas no se mueven nada, ni un píxel. Al moverla más de 0,6° vuelven a seguir el espacio con suavidad. Prueba: si notas que se pegan a la cabeza al empezar a moverla, desactívalo.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                }
                .font(Theme.body).foregroundStyle(Theme.text)
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

            Section(title: "Cine en 3D con IA") {
                Text("Con las gafas en modo 3D, mira el vídeo y pulsa ⌘⌥T: modo cine con relieve. ⌘⌥T otra vez lo quita; ⌘⌥P sale del cine (al volver las gafas a 2D sale solo).")
                    .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                LabeledSlider(title: "Intensidad", value: $model.depthStrength, range: 0.005...0.08, step: 0.0025,
                              caption: String(format: "%.1f %%", model.depthStrength * 100))
                LabeledSlider(title: "Plano de la pantalla", value: $model.depthPlane, range: 0...1, step: 0.05,
                              caption: model.depthPlane < 0.2 ? "todo sale hacia ti"
                                  : model.depthPlane > 0.8 ? "todo hacia el fondo" : String(format: "%.2f", model.depthPlane))
            }

            Section(title: "Atajos de teclado") {
                ShortcutSheet()
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

// Every shortcut, on macOS and on Windows (the same letters with Ctrl+Alt). Global: they work
// with any app in front, while Nimbo runs (the panel and layouts also while it is stopped).
struct ShortcutSheet: View {
    private static let rows: [(mac: String, win: String, what: String)] = [
        ("⌘⌥N", "Ctrl+Alt+N", "Abrir o cerrar el panel de Nimbo"),
        ("⌘⌥1", "Ctrl+Alt+1", "Disposición «1 Pantalla Curva»"),
        ("⌘⌥2", "Ctrl+Alt+2", "Disposición «Doble Monitor»"),
        ("⌘⌥3", "Ctrl+Alt+3", "Disposición «Triple Ultrawide»"),
        ("⌘⌥C", "Ctrl+Alt+C", "Recentrar: las pantallas vuelven delante de ti"),
        ("⌘⌥G", "Ctrl+Alt+G", "Modo fijo de las gafas: ellas sostienen la imagen, quietísima, todo dentro de su imagen (otra vez: espacio a tamaño real)"),
        ("⌘⌥J", "Ctrl+Alt+J", "Llevar el cursor a la pantalla que miras"),
        ("⌘⌥O", "Ctrl+Alt+O", "Modo enfoque: solo la pantalla que miras (otra vez: todas)"),
        ("⌘⌥K", "Ctrl+Alt+K", "Modo código: las pantallas mucho más cerca, zoom 170 % (otra vez: como estaban)"),
        ("⌘⌥+  ⌘⌥-", "Ctrl+Alt++  -", "Zoom: acercar / alejar las pantallas"),
        ("⌘⌥0", "Ctrl+Alt+0", "Zoom al 100 %"),
        ("⌘⌥P", "Ctrl+Alt+P", "Modo cine: la pantalla que miras, delante y grande"),
        ("⌘⌥T", "Ctrl+Alt+T", "Cine en 3D con IA: modo cine con relieve (gafas en modo 3D)"),
        ("⌘⌥R", "—", "Grabar lo que ves en las gafas (vídeo en Descargas)"),
        ("⌘⌥↓  ⌘⌥↑", "Ctrl+Alt+↓  ↑", "Brillo de la pantalla de las gafas: bajar / subir (8 niveles)"),
        ("⌘⌥⇧-  ⌘⌥⇧+", "Ctrl+Alt+Mayús+-  +", "Calibrar el campo de visión: si las pantallas se mueven con la cabeza (-) o contra ella (+)"),
        ("⌘⌥Q", "Ctrl+Alt+Q", "Parar Nimbo (todo vuelve a la normalidad)"),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 7) {
                GridRow {
                    Text("Mac").font(Theme.label).foregroundStyle(Theme.secondary)
                    Text("Windows").font(Theme.label).foregroundStyle(Theme.secondary)
                    Text("Qué hace").font(Theme.label).foregroundStyle(Theme.secondary)
                }
                ForEach(Self.rows, id: \.mac) { row in
                    GridRow {
                        Text(row.mac).font(.system(size: 13, weight: .semibold, design: .rounded)).foregroundStyle(Theme.text)
                        Text(row.win).font(.system(size: 12, design: .rounded)).foregroundStyle(Theme.secondary)
                        Text(row.what).font(.system(size: 12)).foregroundStyle(Theme.text)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            Text("Funcionan con cualquier app delante mientras Nimbo está en marcha; el panel y las disposiciones, también con Nimbo parado.")
                .font(.system(size: 12)).foregroundStyle(Theme.secondary)
        }
    }
}
