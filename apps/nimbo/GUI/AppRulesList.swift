import AppKit
import SwiftUI

// Per-app profiles: pick an app and the profile its look switches to while it is in front.
struct AppRulesList: View {
    @ObservedObject var model: AppModel
    @StateObject private var appsBox = Box<[NSRunningApplication]>([])

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(model.appRules.enumerated()), id: \.offset) { i, rule in
                HStack(spacing: 12) {
                    Text(rule.name).font(Theme.body).foregroundStyle(Theme.text).frame(width: 200, alignment: .leading)
                    Picker("", selection: Binding(get: { model.appRules[safe: i]?.profile ?? "" },
                                                  set: { if model.appRules.indices.contains(i) { model.appRules[i].profile = $0 } })) {
                        ForEach(AppModel.profiles) { Text($0.title).tag($0.id) }
                    }
                    .labelsHidden()
                    .frame(width: 140)
                    if model.activeAppProfile == rule.profile, NSWorkspace.shared.frontmostApplication?.bundleIdentifier == rule.bundleID {
                        Text("activo").font(Theme.small).foregroundStyle(Theme.accent)
                    }
                    Spacer()
                    Button("Quitar") { model.appRules.remove(at: i) }.buttonStyle(CalmButtonStyle())
                }
            }
            HStack(spacing: 12) {
                Menu("Añadir app…") {
                    ForEach(appsBox.value, id: \.processIdentifier) { app in
                        Button(app.localizedName ?? app.bundleIdentifier ?? "?") { add(app) }
                    }
                }
                .frame(width: 200)
                Button("Actualizar") { refresh() }.buttonStyle(CalmButtonStyle())
                Spacer()
            }
            Text("Mientras esa app está delante, Nimbo usa la distancia, la curvatura y el tamaño de su perfil; al cambiar de app vuelve a los tuyos. El número de monitores no cambia.")
                .font(Theme.small).foregroundStyle(Theme.secondary)
        }
        .onAppear { refresh() }
    }

    private func refresh() {
        let mine = Bundle.main.bundleIdentifier
        let used = Set(model.appRules.map(\.bundleID))
        appsBox.value = NSWorkspace.shared.runningApplications
            .filter { $0.activationPolicy == .regular && $0.bundleIdentifier != nil && $0.bundleIdentifier != mine }
            .filter { !used.contains($0.bundleIdentifier!) }
            .sorted { ($0.localizedName ?? "") < ($1.localizedName ?? "") }
    }

    private func add(_ app: NSRunningApplication) {
        guard let id = app.bundleIdentifier else { return }
        model.appRules.append(.init(bundleID: id, name: app.localizedName ?? id, profile: AppModel.profiles.first?.id ?? "code"))
        refresh()
    }
}
