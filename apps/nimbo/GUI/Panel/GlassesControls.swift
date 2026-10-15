import SwiftUI

// The glasses' own mode and brightness, set by Nimbo over USB (see docs/PROTOCOL.md: 0x08, 0x09).
struct GlassesModePicker: View {
    @ObservedObject var model: AppModel
    var showLabel = true

    var body: some View {
        Picker("Modo de las gafas", selection: $model.glassesMode) {
            ForEach(0..<AppModel.glassesModeNames.count, id: \.self) { Text(AppModel.glassesModeNames[$0]).tag($0) }
        }
        .pickerStyle(.segmented)
        .labelsHidden(!showLabel)
    }
}

private extension View {
    @ViewBuilder func labelsHidden(_ hidden: Bool) -> some View {
        if hidden { labelsHidden() } else { self }
    }
}

struct BrightnessSlider: View {
    @ObservedObject var model: AppModel

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "sun.min").foregroundStyle(.secondary)
            Slider(value: Binding(get: { Double(model.brightness == 0 ? 8 : model.brightness) },
                                  set: { model.brightness = Int($0.rounded()) }),
                   in: 1...8, step: 1)
                .tint(Color(red: 0.55, green: 0.72, blue: 0.70))
            Image(systemName: "sun.max").foregroundStyle(.secondary)
        }
    }
}
