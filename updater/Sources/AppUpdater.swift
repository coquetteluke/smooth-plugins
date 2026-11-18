import AppKit
import CryptoKit

/// Replaces this app with the latest release and opens the new version.
enum AppUpdater {
    enum Failure: LocalizedError {
        case temporaryLocation
        case noChecksum
        case wrongApp

        var errorDescription: String? {
            switch self {
            case .temporaryLocation: "move Smooth Plugins into your Applications folder first, then update"
            case .noChecksum: "this release has no checksum for the app; download it from the releases page"
            case .wrongApp: "the download did not contain the expected version of the app"
            }
        }
    }

    /// Whether the app can replace itself where it is (not in the temporary copy macOS runs from a download).
    static var canUpdateInPlace: Bool { !DailyCheck.appIsInTemporaryLocation }

    /// Downloads and checks the release, then swaps it in for the running app's bundle.
    @concurrent
    static func install(_ release: AppRelease) async throws {
        guard canUpdateInPlace else { throw Failure.temporaryLocation }
        guard PluginInstaller.isTrusted(release.url) else { throw InstallError.untrustedLink }
        guard let expected = release.sha256?.lowercased() else { throw Failure.noChecksum }

        let fileManager = FileManager.default
        let work = fileManager.temporaryDirectory.appending(path: "SmoothPlugins-app-\(UUID().uuidString)", directoryHint: .isDirectory)
        try fileManager.createDirectory(at: work, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: work) }

        let (downloaded, _) = try await URLSession.shared.download(from: release.url)
        let zip = work.appending(path: "app.zip")
        try fileManager.moveItem(at: downloaded, to: zip)
        let digest = SHA256.hash(data: try Data(contentsOf: zip)).map { String(format: "%02x", $0) }.joined()
        guard digest == expected else { throw InstallError.checksumMismatch }

        let unpacked = work.appending(path: "unpacked", directoryHint: .isDirectory)
        try Shell.run("/usr/bin/ditto", ["-x", "-k", zip.path, unpacked.path])
        let newApp = unpacked.appending(path: "Smooth Plugins.app", directoryHint: .isDirectory)
        guard
            let data = try? Data(contentsOf: newApp.appending(path: "Contents/Info.plist")),
            let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
            info["CFBundleIdentifier"] as? String == Bundle.main.bundleIdentifier,
            info["CFBundleShortVersionString"] as? String == release.version
        else {
            throw Failure.wrongApp
        }
        try? Shell.run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", newApp.path])
        try Shell.run("/usr/bin/codesign", ["--verify", "--deep", "--strict", newApp.path])

        let current = Bundle.main.bundleURL
        let folder = current.deletingLastPathComponent()
        if fileManager.isWritableFile(atPath: folder.path) {
            let staged = folder.appending(path: ".Smooth Plugins update.app", directoryHint: .isDirectory)
            try? fileManager.removeItem(at: staged)
            try Shell.run("/usr/bin/ditto", [newApp.path, staged.path])
            _ = try fileManager.replaceItemAt(current, withItemAt: staged)
        } else {
            try await Admin.replace(current, with: newApp)
        }
    }

    /// Opens the app again once this copy has quit, then quits.
    @MainActor
    static func relaunch() {
        var openArguments = ["/usr/bin/open"]
        if Config.isTesting {
            for (key, value) in ProcessInfo.processInfo.environment where key.hasPrefix("SMOOTH_") {
                openArguments += ["--env", "\(key)=\(value)"]
            }
        }
        openArguments.append(Bundle.main.bundlePath)
        let script = "while /bin/kill -0 \(getpid()) 2>/dev/null; do /bin/sleep 0.2; done; "
            + openArguments.map(Shell.quote).joined(separator: " ")
        let process = Process()
        process.executableURL = URL(filePath: "/bin/sh")
        process.arguments = ["-c", script]
        try? process.run()
        NSApplication.shared.terminate(nil)
    }
}
