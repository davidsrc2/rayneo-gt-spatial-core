import SwiftUI

// Local view state without the SwiftUI @State macro (its plugin ships only with Xcode;
// the build uses the Command Line Tools).
final class Box<T>: ObservableObject {
    @Published var value: T
    init(_ value: T) { self.value = value }
}

// Calm palette: low contrast, desaturated, no reds. Everything slow and soft.
enum Theme {
    static let background = Color(red: 0.10, green: 0.11, blue: 0.12)
    static let surface = Color(red: 0.14, green: 0.15, blue: 0.17)
    static let card = Color(red: 0.36, green: 0.44, blue: 0.52)
    static let cardMain = Color(red: 0.42, green: 0.55, blue: 0.56)
    static let accent = Color(red: 0.50, green: 0.66, blue: 0.63)
    static let text = Color(white: 0.88)
    static let secondary = Color(white: 0.60)
    static let line = Color(white: 1.0, opacity: 0.08)

    static let body = Font.system(size: 16)
    static let label = Font.system(size: 15)
    static let title = Font.system(size: 22, weight: .regular)
    static let small = Font.system(size: 14)

    static let ease = Animation.easeInOut(duration: 0.35)
}

struct CalmButtonStyle: ButtonStyle {
    var prominent = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Theme.body)
            .padding(.horizontal, prominent ? 28 : 18)
            .padding(.vertical, prominent ? 13 : 9)
            .foregroundStyle(prominent ? Color.black.opacity(0.8) : Theme.text)
            .background(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(prominent ? Theme.accent : Theme.surface)
            )
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(Theme.line))
            .opacity(configuration.isPressed ? 0.75 : 1)
            .animation(Theme.ease, value: configuration.isPressed)
    }
}

// Segmented choice in the calm palette (the system control uses the bright accent colour).
struct CalmSegmented: View {
    let options: [String]
    @Binding var selection: Int

    var body: some View {
        HStack(spacing: 4) {
            ForEach(Array(options.enumerated()), id: \.offset) { i, title in
                Text(title)
                    .font(Theme.body)
                    .foregroundStyle(i == selection ? Color.black.opacity(0.8) : Theme.text)
                    .padding(.horizontal, 20)
                    .padding(.vertical, 8)
                    .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                        .fill(i == selection ? Theme.accent : Color.clear))
                    .contentShape(Rectangle())
                    .onTapGesture { withAnimation(Theme.ease) { selection = i } }
            }
        }
        .padding(4)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Theme.background))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(Theme.line))
    }
}

struct Section<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(title).font(Theme.label).foregroundStyle(Theme.secondary)
            content
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Theme.surface))
    }
}
