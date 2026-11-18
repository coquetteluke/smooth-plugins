import SwiftUI

struct PluginListView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        List(model.rows) { row in
            PluginRowView(row: row, isPicked: $model[picked: row.id])
                .disabled(model.isBusy)
        }
        .listStyle(.inset(alternatesRowBackgrounds: true))
        .overlay {
            if model.rows.isEmpty {
                switch model.phase {
                case .checking:
                    ProgressView("Checking for updates…")
                case .failed:
                    VStack(spacing: 10) {
                        Text("Couldn't reach GitHub. Check your internet connection.")
                            .foregroundStyle(.secondary)
                        Button("Try Again") {
                            Task { await model.refresh() }
                        }
                    }
                case .ready:
                    EmptyView()
                }
            }
        }
    }
}
