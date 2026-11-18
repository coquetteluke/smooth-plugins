import SwiftUI

struct StatusBadge: View {
    let status: PluginRow.Status

    var body: some View {
        Text(title)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .foregroundStyle(color)
            .background(color.opacity(0.15), in: Capsule())
    }

    private var title: String {
        switch status {
        case .notInstalled: "Not installed"
        case .upToDate: "Up to date"
        case .updateAvailable: "Update"
        }
    }

    private var color: Color {
        switch status {
        case .notInstalled: .secondary
        case .upToDate: .green
        case .updateAvailable: .orange
        }
    }
}
