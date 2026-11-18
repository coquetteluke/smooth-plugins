import SwiftUI

/// A one-line notice under the header, with an optional button.
struct NoticeView: View {
    let symbol: String
    let text: String
    var buttonTitle: String?
    var action: (() -> Void)?

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: symbol)
                .foregroundStyle(.tint)
                .accessibilityHidden(true)
            Text(text)
            Spacer()
            if let buttonTitle, let action {
                Button(buttonTitle, action: action)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(.tint.opacity(0.1))
    }
}
