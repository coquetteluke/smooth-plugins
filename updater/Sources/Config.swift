import Foundation

/// Where things live. The SMOOTH_* environment variables exist only for testing the app
/// against a local release and a scratch Components folder.
enum Config {
    static let releasesPage = URL(string: "https://github.com/coquetteluke/smooth-plugins/releases/latest")!
    static let repoPage = URL(string: "https://github.com/coquetteluke/smooth-plugins")!

    /// Plug-ins are only ever downloaded from this repo's releases.
    static let downloadPrefix = "https://github.com/coquetteluke/smooth-plugins/releases/download/"

    static let logicBundleID = "com.apple.logic10"

    private static let environment = ProcessInfo.processInfo.environment

    static var isTesting: Bool {
        environment["SMOOTH_MANIFEST_URL"] != nil || environment["SMOOTH_COMPONENTS_DIR"] != nil
    }

    static var manifestURL: URL {
        if let value = environment["SMOOTH_MANIFEST_URL"], let url = URL(string: value) {
            return url
        }
        return URL(string: "https://github.com/coquetteluke/smooth-plugins/releases/latest/download/manifest.json")!
    }

    /// Plug-ins installed "for me", and where new installs go.
    static var userComponents: URL {
        if let value = environment["SMOOTH_COMPONENTS_DIR"] {
            return URL(filePath: value, directoryHint: .isDirectory)
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appending(path: "Library/Audio/Plug-Ins/Components", directoryHint: .isDirectory)
    }

    /// Plug-ins installed "for all users". Changing these needs an administrator password.
    static var systemComponents: URL? {
        if let value = environment["SMOOTH_SYSTEM_COMPONENTS_DIR"] {
            return URL(filePath: value, directoryHint: .isDirectory)
        }
        if isTesting {
            return nil
        }
        return URL(filePath: "/Library/Audio/Plug-Ins/Components", directoryHint: .isDirectory)
    }

    static var componentFolders: [(url: URL, scope: InstalledCopy.Scope)] {
        var folders: [(url: URL, scope: InstalledCopy.Scope)] = [(userComponents, .me)]
        if let systemComponents {
            folders.append((systemComponents, .allUsers))
        }
        return folders
    }

    static var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
    }
}
