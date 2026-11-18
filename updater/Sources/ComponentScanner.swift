import Foundation

/// Finds the installed copies of a plug-in, by its file name and bundle identifier.
enum ComponentScanner {
    static func copies(of plugin: PluginRelease) -> [InstalledCopy] {
        Config.componentFolders.compactMap { folder in
            let bundle = folder.url.appending(path: "\(plugin.name).component", directoryHint: .isDirectory)
            let infoURL = bundle.appending(path: "Contents/Info.plist")
            guard
                let data = try? Data(contentsOf: infoURL),
                let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
                info["CFBundleIdentifier"] as? String == plugin.bundleId
            else {
                return nil
            }
            let version = info["CFBundleShortVersionString"] as? String ?? "0"
            return InstalledCopy(url: bundle, version: version, scope: folder.scope)
        }
    }
}
