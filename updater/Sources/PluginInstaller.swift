import CryptoKit
import Foundation

/// Downloads, checks, installs and removes plug-ins.
enum PluginInstaller {
    /// Installs the release of `plugin`, replacing every installed copy, or installing it for the
    /// current user when there is none.
    @concurrent
    static func install(_ plugin: PluginRelease, replacing copies: [InstalledCopy]) async throws {
        guard isTrusted(plugin.url) else {
            throw InstallError.untrustedLink
        }
        let fileManager = FileManager.default
        let work = fileManager.temporaryDirectory.appending(path: "SmoothPlugins-\(UUID().uuidString)", directoryHint: .isDirectory)
        try fileManager.createDirectory(at: work, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: work) }

        let (downloaded, _) = try await URLSession.shared.download(from: plugin.url)
        let zip = work.appending(path: "download.zip")
        try fileManager.moveItem(at: downloaded, to: zip)
        guard try sha256(of: zip) == plugin.sha256.lowercased() else {
            throw InstallError.checksumMismatch
        }

        let unpacked = work.appending(path: "unpacked", directoryHint: .isDirectory)
        try Shell.run("/usr/bin/ditto", ["-x", "-k", zip.path, unpacked.path])
        let bundle = unpacked.appending(path: "\(plugin.name).component", directoryHint: .isDirectory)
        guard bundleIdentifier(of: bundle) == plugin.bundleId else {
            throw InstallError.unexpectedContents
        }
        try? Shell.run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", bundle.path])

        let targets = copies.isEmpty
            ? [InstalledCopy(url: Config.userComponents.appending(path: "\(plugin.name).component", directoryHint: .isDirectory), version: "", scope: .me)]
            : copies
        for target in targets {
            switch target.scope {
            case .me:
                try replaceForMe(target.url, with: bundle)
            case .allUsers:
                try await Admin.install(zip: zip, sha256: plugin.sha256, bundleName: "\(plugin.name).component", at: target.url)
            }
        }
    }

    /// Moves copies installed for the current user to the Trash and deletes copies installed for all users.
    @concurrent
    static func remove(_ copies: [InstalledCopy]) async throws {
        for copy in copies {
            switch copy.scope {
            case .me:
                try FileManager.default.trashItem(at: copy.url, resultingItemURL: nil)
            case .allUsers:
                try await Admin.remove(copy.url)
            }
        }
    }

    /// Lets hosts that are already open see the changed plug-ins.
    static func refreshHosts() {
        try? Shell.run("/usr/bin/killall", ["-9", "AudioComponentRegistrar"])
    }

    static func isTrusted(_ url: URL) -> Bool {
        if url.absoluteString.hasPrefix(Config.downloadPrefix) {
            return true
        }
        return Config.isTesting && url.isFileURL
    }

    private static func replaceForMe(_ target: URL, with bundle: URL) throws {
        let fileManager = FileManager.default
        let folder = target.deletingLastPathComponent()
        try fileManager.createDirectory(at: folder, withIntermediateDirectories: true)
        let staged = folder.appending(path: ".\(target.lastPathComponent).new", directoryHint: .isDirectory)
        try? fileManager.removeItem(at: staged)
        try Shell.run("/usr/bin/ditto", [bundle.path, staged.path])
        if fileManager.fileExists(atPath: target.path) {
            _ = try fileManager.replaceItemAt(target, withItemAt: staged)
        } else {
            try fileManager.moveItem(at: staged, to: target)
        }
    }

    private static func sha256(of file: URL) throws -> String {
        let digest = SHA256.hash(data: try Data(contentsOf: file))
        return digest.map { String(format: "%02x", $0) }.joined()
    }

    private static func bundleIdentifier(of bundle: URL) -> String? {
        guard
            let data = try? Data(contentsOf: bundle.appending(path: "Contents/Info.plist")),
            let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
        else {
            return nil
        }
        return info["CFBundleIdentifier"] as? String
    }
}
