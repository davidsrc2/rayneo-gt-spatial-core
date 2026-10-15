import SwiftUI

// Single windows as panels of their own (a video above the monitors, a chat to the side).
// Pick one from the open windows; it appears centred above the monitors and can be moved and
// resized in "Colocar a mano". The real window keeps working where it is; the panel shows it.
struct FloatingWindowsList: View {
    @ObservedObject var model: AppModel
    let running: Bool
    @StateObject private var windowsBox = Box<[ScreenCapture.WindowInfo]>([])

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(model.floating.enumerated()), id: \.offset) { i, w in
                HStack(spacing: 10) {
                    Text("V\(i + 1)").font(Theme.label).foregroundStyle(Theme.accent).frame(width: 28, alignment: .leading)
                    Text(w.title.isEmpty ? w.match : w.title)
                        .font(Theme.body).foregroundStyle(Theme.text).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    Button("Quitar") { model.floating.remove(at: i) }.buttonStyle(CalmButtonStyle())
                }
            }
            HStack(spacing: 12) {
                Menu("Añadir ventana…") {
                    if windowsBox.value.isEmpty { Text("No hay ventanas (pulsa Actualizar)") }
                    ForEach(windowsBox.value, id: \.self) { w in
                        Button(w.title.isEmpty ? w.app : "\(w.app) — \(w.title)") {
                            model.floating.append(FloatingWindow(windowID: w.id, title: w.title.isEmpty ? w.app : "\(w.app) — \(w.title)",
                                                                 app: w.app, windowTitle: w.title))
                        }
                    }
                }
                .frame(width: 200)
                .disabled(model.floating.count + model.cells.count >= 8)
                Button("Actualizar") { refresh() }.buttonStyle(CalmButtonStyle())
                Spacer()
            }
            Text("Aparecen encima de los monitores; muévelas o cambia su tamaño en «Colocar a mano». Se recuerdan: al empezar, Nimbo vuelve a buscar cada una por su app. Mirando una, ⌘⌥J trae la ventana real al frente con el cursor encima." + (running ? "" : " Se muestran al empezar."))
                .font(Theme.small).foregroundStyle(Theme.secondary)
        }
        .onAppear { refresh() }
    }

    private func refresh() {
        Task { @MainActor in
            let used = Set(model.floating.map(\.windowID))
            windowsBox.value = await ScreenCapture.windows().filter { !used.contains($0.id) }
        }
    }
}
