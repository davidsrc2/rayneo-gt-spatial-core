import SwiftUI

// The glasses' brightness, set by Nimbo over USB (docs/PROTOCOL.md: 0x09). Their mode stays on
// their own button.
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
