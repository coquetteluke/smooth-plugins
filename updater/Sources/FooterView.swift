import SwiftUI

struct FooterView: View {
    @ObservedObject var model: AppModel
    @State private var confirmingRemove = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                Text("Pick:")
                    .foregroundStyle(.secondary)
                Button("All") { model.pick { _ in true } }
                Button("Updates") { model.pick { $0.status == .updateAvailable } }
                Button("None") { model.pick { _ in false } }
                Spacer()
                Toggle("Check for updates every day", isOn: $model.dailyCheck)
                    .disabled(!model.canScheduleDailyCheck)
            }
            .buttonStyle(.link)
            .disabled(model.isBusy || model.rows.isEmpty)

            Text(model.canScheduleDailyCheck
                 ? "When updates come out, Smooth Plugins opens in the background with the number of updates on its Dock icon."
                 : "Move Smooth Plugins into your Applications folder and open it from there to turn on daily checks.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .trailing)

            HStack(spacing: 10) {
                if let busy = model.busyMessage {
                    ProgressView()
                        .controlSize(.small)
                    Text(busy)
                } else if let result = model.resultMessage {
                    Text(result)
                        .lineLimit(3)
                        .textSelection(.enabled)
                }
                Spacer()
                Button("Remove…") { confirmingRemove = true }
                    .disabled(model.toRemove.isEmpty || model.isBusy)
                Button(primaryTitle) {
                    Task { await model.installPicked() }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(model.toInstall.isEmpty || model.isBusy)
            }
        }
        .padding(16)
        .confirmationDialog(removeTitle, isPresented: $confirmingRemove) {
            Button("Remove", role: .destructive) {
                Task { await model.removePicked() }
            }
        } message: {
            Text("Plug-ins installed for you go to the Trash. Plug-ins installed for all users are deleted and need your password.")
        }
    }

    private var primaryTitle: String {
        let rows = model.toInstall
        if rows.isEmpty {
            return "Install"
        }
        let updates = rows.filter { $0.status == .updateAvailable }.count
        if updates == rows.count {
            return "Update \(rows.count)"
        }
        if updates == 0 {
            return "Install \(rows.count)"
        }
        return "Install and Update \(rows.count)"
    }

    private var removeTitle: String {
        "Remove \(DailyCheck.sentence(model.toRemove.map(\.plugin.name)))?"
    }
}
