/// A plug-in as the window shows it: the latest release next to what is installed.
struct PluginRow: Identifiable, Sendable {
    enum Status: Sendable {
        case notInstalled
        case upToDate
        case updateAvailable
    }

    let plugin: PluginRelease
    let copies: [InstalledCopy]

    var id: String { plugin.id }

    var installedVersion: String? {
        copies.map(\.version).min { Version.isNewer($1, than: $0) }
    }

    var status: Status {
        if copies.isEmpty {
            return .notInstalled
        }
        let outdated = copies.contains { Version.isNewer(plugin.version, than: $0.version) }
        return outdated ? .updateAvailable : .upToDate
    }

    init(plugin: PluginRelease) {
        self.plugin = plugin
        self.copies = ComponentScanner.copies(of: plugin)
    }
}
