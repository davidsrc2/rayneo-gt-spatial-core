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
        .modifier(ApplyOnChange(model: model, server: server))
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
                Text(model.hasCalibration ? "Tus gafas están calibradas." : "Calibración de fábrica activa (RayNeo GT).")
                    .font(Theme.body).foregroundStyle(Theme.text)
                HStack(spacing: 12) {
                    Button("Volver a calibrar", action: onRecalibrate).buttonStyle(CalmButtonStyle())
                    if model.hasCalibration {
                        Button("Restablecer a fábrica") {
                            model.resetToFactoryCalibration(server: server)
                        }.buttonStyle(CalmButtonStyle())
                    }
                }
                Button("Ajustar la óptica (⌘⌥L)") { server.startOpticsWizard() }.buttonStyle(CalmButtonStyle())
                    .disabled(server.state == .stopped)
                Text("Con Nimbo en marcha y las gafas puestas: gira la cabeza despacio mirando el borde de una pantalla y responde si se mueve con la cabeza (⌘⌥⇧−) o en contra (⌘⌥⇧+). En unos 20 s las pantallas quedan quietas al girar.")
                    .font(.system(size: 12)).foregroundStyle(Theme.secondary)
            }

            Section(title: "Pantallas") {
                Toggle("Atenuar los monitores que no miras", isOn: $model.dimOthers)
                    .font(Theme.body).foregroundStyle(Theme.text)
                Text("Cada pantalla mide lo que la imagen de las gafas y queda quieta en la habitación: gira la cabeza para pasar de una a otra. Nimbo pone las gafas en follow y, al parar, las devuelve a pinned. ⌘⌥C las vuelve a poner delante.")
                    .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                VStack(alignment: .leading, spacing: 6) {
                    Toggle("Pared continua", isOn: $model.flatWall)
                    Text("Todas las pantallas en un mismo plano, rectas y pegadas borde con borde, como un muro de monitores. Desactivado: cada pantalla girada hacia ti, con su separación, curvatura e inclinación.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                    Toggle("Resaltar la pantalla que recibe el teclado", isOn: $model.keyHighlight)
                    Text("Un marco fino de color alrededor de la pantalla donde escribes (la ventana activa), para no escribir en la que no es.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                    Toggle("Modo vehículo", isOn: $model.vehicleMode)
                    Text("Para ir en coche, tren o avión: las pantallas siguen solo una cuarta parte de los giros, así acompañan las curvas del vehículo en vez de quedarse atrás. Mirar a los lados mueve menos las pantallas.")
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                    Toggle("Anclar las pantallas con la cabeza quieta", isOn: $model.anchorAtRest)
                    Text("En el espacio a tamaño real el anclaje va siempre incluido: con la cabeza quieta las pantallas se congelan; al moverla (más de 0,15° o de 1,4°/s) pasan en 40 ms a seguir el espacio 1:1, y 150 ms después de parar se congelan de nuevo.")
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
                    Toggle("Modo fijo (⌘⌥G): cambiar de pantalla mirándola", isOn: $model.growByGaze)
                    Text("En el modo fijo de las gafas, la pantalla que miras pasa a primer plano. Desactivado: solo la que eliges con el ratón.")
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

            Section(title: "Fondo alrededor (⌘⌥F)") {
                BackgroundSettings(model: model, server: server)
            }

            Section(title: "Atajos de teclado") {
                ShortcutSheet()
            }

            Section(title: "Diagnóstico") {
                DiagnosticsView(server: server)
            }

            Section(title: "Actualizaciones") {
                UpdatesRow(updater: Updater.shared)
            }

            DisclosureGroup(isExpanded: $advancedBox.value) {
                VStack(alignment: .leading, spacing: 14) {
                    if model.glassesPinned {
                        LabeledSlider(title: "Campo de visión", value: $model.fov, range: 15...35, step: 0.5,
                                      caption: String(format: "%.1f°", model.fov))
                    } else {  // the space at real size: the optics', fine-tuned within their range
                        LabeledSlider(title: "Campo de visión óptico",
                                      value: Binding(get: { model.opticalVFov }, set: { model.opticalVFov = $0; server.settingsChanged() }),
                                      range: Double(NormalMode.opticalVFovRange.lowerBound)...Double(NormalMode.opticalVFovRange.upperBound),
                                      step: 0.1, caption: String(format: "%.1f°", model.opticalVFov))
                    }
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
        ("⌘⌥A", "Ctrl+Alt+A", "Ver de lejos todas las pantallas (otra vez: como estabas)"),
        ("⌘⌥P", "Ctrl+Alt+P", "Modo cine: la pantalla que miras, delante y grande"),
        ("⌘⌥F", "Ctrl+Alt+F", "Fondo alrededor de las pantallas: ninguno, lluvia, estrellas, atardecer, aurora o tu imagen"),
        ("⌘⌥R", "—", "Grabar lo que ves en las gafas (vídeo en Descargas)"),
        ("⌘⌥↓  ⌘⌥↑", "Ctrl+Alt+↓  ↑", "Brillo de la pantalla de las gafas: bajar / subir (8 niveles)"),
        ("⌘⌥⇧-  ⌘⌥⇧+", "Ctrl+Alt+Mayús+-  +", "Calibrar el campo de visión: si las pantallas se mueven con la cabeza (-) o contra ella (+)"),
        ("⌘⌥V", "Ctrl+Alt+V", "Modo vehículo: las pantallas acompañan los giros del coche, tren o avión (otra vez: desactivado)"),
        ("⌘⌥L", "Ctrl+Alt+L", "Ajustar la óptica: asistente que deja las pantallas quietas al girar la cabeza"),
        ("⌘⌥H", "Ctrl+Alt+H", "Todos los atajos, en las gafas (Windows: en una ventana)"),
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

// Around the panels: none (the room seen through the glasses), Nimbo's own or a picture. Each
// change applied at once (bindings that tell the server, not more .onChange: the settings
// chain is at the type checker's limit).
@available(macOS 14.0, *)
struct BackgroundSettings: View {
    @ObservedObject var model: AppModel
    @ObservedObject var server: ServerController

    private func live<T>(_ key: ReferenceWritableKeyPath<AppModel, T>) -> Binding<T> {
        Binding(get: { model[keyPath: key] }, set: { model[keyPath: key] = $0; server.settingsChanged() })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("Fondo", selection: live(\.background)) {
                ForEach(Array(AppModel.backgrounds.enumerated()), id: \.offset) { i, name in
                    Text(name).tag(i)
                }
            }
            .frame(maxWidth: 360)
            if model.background == 5 {
                HStack(spacing: 10) {
                    Button("Elegir imagen…") { choose() }.buttonStyle(CalmButtonStyle())
                    Text(model.backgroundImage.isEmpty ? "Ninguna elegida" : (model.backgroundImage as NSString).lastPathComponent)
                        .font(.system(size: 12)).foregroundStyle(Theme.secondary).lineLimit(1).truncationMode(.middle)
                }
            }
            if model.background > 0 {
                LabeledSlider(title: "Intensidad", value: live(\.backgroundBrightness), range: 0.1...1, step: 0.05,
                              caption: String(format: "%.0f %%", model.backgroundBrightness * 100))
            }
            Text("Fijo en el espacio como las pantallas. En las gafas, el negro es transparente: con «Ninguno» ves la habitación; con un fondo, la tapas. Una panorámica 2:1 rodea toda la habitación; otra foto ocupa 180° y se refleja detrás.")
                .font(.system(size: 12)).foregroundStyle(Theme.secondary)
        }
        .font(Theme.body).foregroundStyle(Theme.text)
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.image]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        model.backgroundImage = url.path
        server.settingsChanged()
    }
}

// "Buscar actualizaciones" / "Actualizar a …" and what the updater says.
@available(macOS 14.0, *)
private struct UpdatesRow: View {
    @ObservedObject var updater: Updater

    var body: some View {
        HStack(spacing: 12) {
            if let r = updater.available {
                Button("Actualizar a \(r.version)") { Task { await updater.install() } }
                    .buttonStyle(CalmButtonStyle(prominent: true))
                    .disabled(updater.busy)
            } else {
                Button("Buscar actualizaciones") { Task { await updater.check(quiet: false) } }
                    .buttonStyle(CalmButtonStyle())
                    .disabled(updater.busy)
            }
            Text(updater.status.isEmpty ? "Versión \(Updater.current)" : updater.status)
                .font(.system(size: 12)).foregroundStyle(Theme.secondary)
        }
    }
}


// Ajustes > Diagnóstico: how the glasses are doing, in plain terms, while Nimbo runs; and a
// problem report (the log, settings, calibration, bias map, the last 30 s of the IMU) to send.
@available(macOS 14.0, *)
struct DiagnosticsView: View {
    @ObservedObject var server: ServerController

    private enum Level { case good, fair, poor, info }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let d = server.diagnostics {
                Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                    row("Latencia (cabeza → imagen)", String(format: "%.0f ms", d.latencyMs),
                        d.latencyMs <= 0 ? .info : d.latencyMs <= 22 ? .good : d.latencyMs <= 30 ? .fair : .poor)
                    row("Imágenes que llegan tarde", "\(d.missedPerS) por segundo",
                        d.missedPerS <= 2 ? .good : d.missedPerS <= 10 ? .fair : .poor)
                    row("Refresco", "\(d.fps) Hz", .info)
                    row("GPU por imagen", String(format: "%.1f ms", d.gpuMs), d.gpuMs <= 3 ? .good : d.gpuMs <= 5 ? .fair : .poor)
                    row("Nitidez del texto", d.supersample >= 2 ? "máxima" : "reducida un momento (fluidez)",
                        d.supersample >= 2 ? .good : .fair)
                    row("Ruido del sensor", String(format: "%.2f °/s", d.gyroNoise),
                        d.gyroNoise <= 0 ? .info : d.gyroNoise < 0.25 ? .good : d.gyroNoise < 0.5 ? .fair : .poor)
                    row("Temperatura de las gafas", String(format: "%.1f °C", d.tempC),
                        d.tempC <= 0 ? .info : d.tempC >= 28 ? .good : .fair)
                    row("Estabilidad del sensor (último minuto)", String(format: "%.2f °/s", d.biasChange),
                        d.biasChange < 0.05 ? .good : d.biasChange < 0.15 ? .fair : .poor)
                    row("Gafas puestas", d.worn ? "sí" : "no (en pausa)", .info)
                    row("Cortes del sensor recuperados", "\(d.stalls)", d.stalls == 0 ? .good : .fair)
                }
            } else {
                Text("Con Nimbo en marcha verás aquí cómo van la imagen y el sensor.")
                    .font(Theme.body).foregroundStyle(Theme.secondary)
            }
            Button("Enviar informe") { server.sendReport() }.buttonStyle(CalmButtonStyle())
            Text("Crea un zip en Descargas con el registro, tus ajustes, la calibración y los últimos 30 s del sensor, y abre un correo con él adjunto.")
                .font(.system(size: 12)).foregroundStyle(Theme.secondary)
        }
    }

    @ViewBuilder
    private func row(_ label: String, _ value: String, _ level: Level) -> some View {
        GridRow {
            Circle().fill(color(level)).frame(width: 8, height: 8)
            Text(label).font(Theme.body).foregroundStyle(Theme.text)
            Text(value).font(Theme.body.monospacedDigit()).foregroundStyle(Theme.secondary)
        }
    }

    private func color(_ l: Level) -> Color {
        switch l {
        case .good: return Color(red: 0.48, green: 0.78, blue: 0.60)
        case .fair: return Color(red: 0.89, green: 0.71, blue: 0.40)
        case .poor: return Color(red: 0.88, green: 0.42, blue: 0.40)
        case .info: return Color(white: 0.55)
        }
    }
}


// Every setting that applies live: the server told when it changes. Split in two groups (and out
// of SettingsView.body): one long modifier chain was more than older Swift compilers could
// type-check in reasonable time.
@available(macOS 14.0, *)
private struct ApplyOnChange: ViewModifier {
    @ObservedObject var model: AppModel
    let server: ServerController

    func body(content: Content) -> some View {
        content
            .onChange(of: model.fov) { server.settingsChanged() }
            .onChange(of: model.deadZone) { server.settingsChanged() }
            .onChange(of: model.gazeCursor) { server.settingsChanged() }
            .onChange(of: model.dimOthers) { server.settingsChanged() }
            .onChange(of: model.tracking) { server.settingsChanged() }
            .onChange(of: model.focusWindows) { server.settingsChanged() }
            .onChange(of: model.growByGaze) { server.settingsChanged() }
            .modifier(ApplyOnChangeMore(model: model, server: server))
    }
}

@available(macOS 14.0, *)
private struct ApplyOnChangeMore: ViewModifier {
    @ObservedObject var model: AppModel
    let server: ServerController

    func body(content: Content) -> some View {
        content
            .onChange(of: model.macScreen) { server.settingsChanged() }
            .onChange(of: model.anchorAtRest) { server.settingsChanged() }
            .onChange(of: model.vehicleMode) { server.settingsChanged() }
            .onChange(of: model.flatWall) { server.settingsChanged() }
            .onChange(of: model.keyHighlight) { server.settingsChanged() }
            .onChange(of: model.k1) { server.settingsChanged() }
            .onChange(of: model.k2) { server.settingsChanged() }
    }
}
