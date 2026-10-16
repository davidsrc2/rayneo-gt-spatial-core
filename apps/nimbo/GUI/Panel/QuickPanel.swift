import SwiftUI

// The whole app for everyday use, in the menu bar. Reading order = priority:
// 1 is it working (light + switch), 2 how many screens, 3 recentre, 4 fine-tune, 5 set-and-forget.
@available(macOS 14.0, *)
struct QuickPanel: View {
    @ObservedObject var model: AppModel
    @ObservedObject var server: ServerController
    let openMore: () -> Void
    @StateObject private var showShortcuts = Box(false)  // @State needs a macro plugin the CLT lack

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            grabber
            header
            LayoutCards(model: model, gaze: server.gazePanel) { server.settingsChanged() }
            recenter
            Tile {
                VStack(spacing: 14) {
                    SnapSlider(title: "Curvatura", value: $model.curvature, range: 0...1,
                               anchors: [(0, "Plana"), (0.5, "Suave"), (1, "Envolvente")],
                               format: { $0 < 0.02 ? "Plana" : String(format: "%.0f %%", $0 * 100) },
                               onCommit: server.settingsChanged)
                    SnapSlider(title: "Inclinación", value: $model.tilt, range: 0...1,
                               anchors: [(0, "Rectas"), (0.5, "Algo"), (1, "Hacia ti")],
                               format: { $0 < 0.02 ? "Rectas" : String(format: "%.0f %%", $0 * 100) },
                               onCommit: server.settingsChanged)
                }
            }
            Tile {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Text("Gafas").font(.system(size: 13, weight: .medium))
                        Spacer()
                        Text("Brillo · ⌘⌥[ ]").font(.system(size: 11, design: .rounded)).foregroundStyle(.secondary)
                    }
                    BrightnessSlider(model: model)
                }
            }
            footer
        }
        .padding(18)
        .frame(width: 380)
        .background(Color.black.opacity(0.28))  // keeps the glass dark over bright windows
        .background(Glass())
        .preferredColorScheme(.dark)
        .onChange(of: model.distance) { server.settingsChanged() }
        .onChange(of: model.curvature) { server.settingsChanged() }
        .onChange(of: model.tilt) { server.settingsChanged() }
        .onChange(of: model.size) { server.settingsChanged() }
        .onChange(of: model.brightness) { server.settingsChanged() }
    }

    // Drag here (or on any empty part) to move the panel like a window.
    private var grabber: some View {
        Capsule()
            .fill(.white.opacity(0.25))
            .frame(width: 44, height: 5)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 4)
            .background(WindowDragArea())
            .padding(.top, -10)
            .padding(.bottom, -8)
            .help("Arrastra para mover el panel")
    }

    private var header: some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Nimbo").font(.system(size: 17, weight: .semibold))
                StatusLight(server: server)
            }
            Spacer().frame(maxHeight: .infinity).background(WindowDragArea())
            Toggle("", isOn: Binding(get: { server.isRunning }, set: { $0 ? server.start() : server.stop() }))
                .toggleStyle(.switch)
                .tint(Color(red: 0.48, green: 0.78, blue: 0.60))
                .labelsHidden()
                .disabled(server.state == .starting)
        }
    }

    private var recenter: some View {
        Button { server.recenter() } label: {
            HStack {
                Image(systemName: "scope").font(.system(size: 16))
                Text("Recentrar vista").font(.system(size: 15, weight: .medium))
                Spacer()
                Text("⌘⌥C")
                    .font(.system(size: 12, design: .rounded))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Capsule().fill(.white.opacity(0.1)))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(Color(red: 0.55, green: 0.72, blue: 0.70).opacity(server.isRunning ? 0.32 : 0.12)))
            .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).stroke(.white.opacity(0.12)))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!server.isRunning)
        .opacity(server.isRunning ? 1 : 0.6)
        .animation(.easeInOut(duration: 0.35), value: server.isRunning)
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Toggle(isOn: $model.autoStart) {
                    Text("Auto-iniciar al conectar las RayNeo GT").font(.system(size: 13))
                }
                .toggleStyle(.switch)
                .controlSize(.small)
                Spacer()
            }
            HStack {
                if let notice = server.notice {
                    Text(notice).font(.system(size: 12)).foregroundStyle(.secondary).lineLimit(2)
                }
                Spacer()
                // The cheat sheet of every shortcut (also in Settings), right here.
                Button {
                    showShortcuts.value.toggle()
                } label: {
                    Label("Atajos", systemImage: "keyboard").font(.system(size: 12))
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .popover(isPresented: $showShortcuts.value, arrowEdge: .bottom) {
                    ShortcutSheet()
                        .padding(18)
                        .frame(width: 560)
                        .preferredColorScheme(.dark)
                }
                Button("Más opciones…", action: openMore)
                    .buttonStyle(.plain)
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }
        }
    }
}

// A patch of the panel that drags the whole window, like a title bar.
struct WindowDragArea: NSViewRepresentable {
    final class DragView: NSView {
        override var mouseDownCanMoveWindow: Bool { true }
        override func mouseDown(with event: NSEvent) { window?.performDrag(with: event) }
    }

    func makeNSView(context: Context) -> NSView { DragView() }
    func updateNSView(_ nsView: NSView, context: Context) {}
}
