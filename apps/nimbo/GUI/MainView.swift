import SwiftUI

@available(macOS 14.0, *)
struct MainView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var server: ServerController
    @StateObject private var showSettings = Box(false)
    @StateObject private var showOnboarding = Box(false)

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                header

                Section(title: "Tus monitores") {
                    LayoutPreview(model: model)
                    LayoutEditor(model: model)
                    HStack(spacing: 12) {
                        Button { withAnimation(Theme.ease) { model.removeMonitor() } } label: {
                            Image(systemName: "minus").frame(width: 22)
                        }
                        .buttonStyle(CalmButtonStyle())
                        .disabled(model.cells.count <= 1)
                        Text(monitorCount)
                            .font(Theme.body).foregroundStyle(Theme.text).frame(minWidth: 110)
                        Button { withAnimation(Theme.ease) { model.addMonitor() } } label: {
                            Image(systemName: "plus").frame(width: 22)
                        }
                        .buttonStyle(CalmButtonStyle())
                        .disabled(model.cells.count >= 6)
                        Spacer()
                        Text("Arrastra los monitores para colocarlos")
                            .font(Theme.small).foregroundStyle(Theme.secondary)
                    }
                    HStack(spacing: 12) {
                        TemplateButton(title: "Uno grande", cells: Templates.one) { apply(Templates.one) }
                        TemplateButton(title: "Dos lado a lado", cells: Templates.twoSide) { apply(Templates.twoSide) }
                        TemplateButton(title: "Dos apilados", cells: Templates.twoStacked) { apply(Templates.twoStacked) }
                        TemplateButton(title: "Tres en arco", cells: Templates.threeArc) { apply(Templates.threeArc) }
                    }
                }

                Section(title: "Colocar a mano") {
                    SpatialEditor(model: model)
                }

                Section(title: "Ventanas flotantes") {
                    FloatingWindowsList(model: model, running: server.isRunning)
                }

                Section(title: "Perfiles por app") {
                    AppRulesList(model: model)
                }

                Section(title: "Cómo se ven") {
                    LabeledSlider(title: "Tamaño del texto",
                                  value: Binding(get: { Double(model.textSize) }, set: { model.textSize = Int($0.rounded()) }),
                                  range: 0...Double(AppModel.textSizes.count - 1), step: 1,
                                  caption: AppModel.textSizes[model.textSize].label)
                    LabeledSlider(title: "Distancia", value: $model.distance, range: 1.2...3.0, step: 0.1,
                                  caption: String(format: "%.1f m", model.distance))
                    LabeledSlider(title: "Separación", value: $model.gap, range: 0...6, step: 0.5,
                                  caption: model.gap == 0 ? "Juntos" : String(format: "%.1f°", model.gap))
                    LabeledSlider(title: "Curvatura", value: $model.curvature, range: 0...1, step: 0.05,
                                  caption: model.curvature < 0.05 ? "Plana" : String(format: "%.0f %%", model.curvature * 100))
                    LabeledSlider(title: "Tamaño", value: $model.size, range: 28...60, step: 1,
                                  caption: String(format: "%.0f°", model.size))
                }

                actions
                status
            }
            .padding(28)
        }
        .background(Theme.background)
        .preferredColorScheme(.dark)
        .onAppear {
            server.bind(model)
            if !model.onboardingDone { showOnboarding.value = true }
        }
        .onChange(of: model.cells) { server.settingsChanged() }
        .onChange(of: model.textSize) { server.settingsChanged() }
        .onChange(of: model.distance) { server.settingsChanged() }
        .onChange(of: model.gap) { server.settingsChanged() }
        .onChange(of: model.curvature) { server.settingsChanged() }
        .onChange(of: model.size) { server.settingsChanged() }
        .onChange(of: model.panelPoses) { server.settingsChanged() }
        .onChange(of: model.floating) { server.settingsChanged() }
        .sheet(isPresented: $showSettings.value) {
            SettingsView(model: model, server: server, onRecalibrate: {
                showSettings.value = false
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { showOnboarding.value = true }
            })
        }
        .sheet(isPresented: $showOnboarding.value) {
            OnboardingView(model: model, server: server, startAt: model.onboardingDone ? .calibration : .welcome) {
                model.onboardingDone = true
                showOnboarding.value = false
            }
        }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Nimbo").font(.system(size: 28, weight: .light)).foregroundStyle(Theme.text)
                Text("Monitores virtuales a tu alrededor, para tus RayNeo GT").font(Theme.label).foregroundStyle(Theme.secondary)
            }
            Spacer()
            Button { showSettings.value = true } label: { Label("Ajustes", systemImage: "slider.horizontal.3") }
                .buttonStyle(CalmButtonStyle())
        }
    }

    private var actions: some View {
        HStack(spacing: 14) {
            Button(server.isRunning ? "Parar" : "Empezar") { server.toggle() }
                .buttonStyle(CalmButtonStyle(prominent: true))
                .disabled(server.state == .starting)
            Button { server.recenter() } label: {
                HStack(spacing: 10) {
                    Text("Recentrar")
                    Text("⌘⌥C").font(Theme.small).foregroundStyle(Theme.secondary)
                }
            }
            .buttonStyle(CalmButtonStyle())
            .disabled(!server.isRunning)
            Spacer()
        }
    }

    private var status: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Circle().fill(server.state == .tracking ? Theme.accent : Theme.secondary.opacity(0.6)).frame(width: 8, height: 8)
                Text(server.statusText).font(Theme.label).foregroundStyle(Theme.secondary)
            }
            if let notice = server.notice {
                Text(notice).font(Theme.small).foregroundStyle(Theme.secondary.opacity(0.85)).transition(.opacity)
            }
        }
        .animation(Theme.ease, value: server.notice)
        .animation(Theme.ease, value: server.state)
    }

    private var monitorCount: String {
        let n = model.cells.count
        return n == 1 ? "1 monitor" : "\(n) monitores"
    }

    private func apply(_ cells: [GridPos]) {
        withAnimation(Theme.ease) { model.applyTemplate(cells) }
    }
}

enum Templates {
    static let one = [GridPos(col: 0, row: 0)]
    static let twoSide = [GridPos(col: 0, row: 0), GridPos(col: 1, row: 0)]
    static let twoStacked = [GridPos(col: 0, row: 0), GridPos(col: 0, row: 1)]
    static let threeArc = [GridPos(col: 0, row: 0), GridPos(col: 1, row: 0), GridPos(col: 2, row: 0)]
}

struct LabeledSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let caption: String

    var body: some View {
        HStack(spacing: 16) {
            Text(title).font(Theme.body).foregroundStyle(Theme.text).frame(width: 170, alignment: .leading)
            Slider(value: $value, in: range, step: step).tint(Theme.accent)
            Text(caption).font(Theme.label).foregroundStyle(Theme.secondary).frame(width: 130, alignment: .trailing)
        }
    }
}
