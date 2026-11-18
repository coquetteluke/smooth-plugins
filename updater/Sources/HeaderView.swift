import SwiftUI

struct HeaderView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        HStack(spacing: 14) {
            Image(nsImage: NSApplication.shared.applicationIconImage)
                .resizable()
                .frame(width: 52, height: 52)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text("Smooth Plugins")
                    .font(.title2.bold())
                Text(status)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            Spacer()
            Button {
                Task { await model.refresh() }
            } label: {
                Label("Check Again", systemImage: "arrow.clockwise")
            }
            .disabled(model.phase == .checking || model.isBusy)
        }
        .padding(16)
    }

    private var status: String {
        switch model.phase {
        case .checking:
            return "Checking for updates…"
        case .failed(let message):
            return "Couldn't check for updates: \(message)"
        case .ready:
            let updates = model.updateCount
            if updates > 0 {
                return updates == 1 ? "1 update available" : "\(updates) updates available"
            }
            if model.rows.allSatisfy({ $0.status == .notInstalled }) {
                return "\(model.rows.count) free plug-ins for Logic Pro only"
            }
            return "Everything is up to date"
        }
    }
}
