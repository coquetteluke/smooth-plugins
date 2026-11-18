import SwiftUI

struct PluginRowView: View {
    let row: PluginRow
    @Binding var isPicked: Bool

    var body: some View {
        HStack(spacing: 12) {
            Toggle(row.plugin.name, isOn: $isPicked)
                .toggleStyle(.checkbox)
                .labelsHidden()
            VStack(alignment: .leading, spacing: 3) {
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(row.plugin.name)
                        .font(.headline)
                    Text(row.plugin.kind)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Text(row.plugin.description)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            Spacer(minLength: 12)
            VStack(alignment: .trailing, spacing: 4) {
                StatusBadge(status: row.status)
                Text(versionText)
                    .font(.caption)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .accessibilityLabel(versionDescription)
            }
        }
        .padding(.vertical, 6)
    }

    private var versionText: String {
        switch row.status {
        case .notInstalled: "Latest \(row.plugin.version)"
        case .upToDate: "Version \(row.installedVersion ?? row.plugin.version)"
        case .updateAvailable: "\(row.installedVersion ?? "?") → \(row.plugin.version)"
        }
    }

    private var versionDescription: String {
        switch row.status {
        case .notInstalled: "Latest version \(row.plugin.version)"
        case .upToDate: "Installed version \(row.installedVersion ?? row.plugin.version)"
        case .updateAvailable: "Installed version \(row.installedVersion ?? "unknown"), new version \(row.plugin.version)"
        }
    }
}
