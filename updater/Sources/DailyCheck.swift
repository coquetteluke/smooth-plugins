import AppKit

/// The once-a-day update check. A launchd agent in ~/Library/LaunchAgents runs this app with
/// --check-updates. When installed plug-ins (or this app) have new versions it has not announced
/// before, it opens the app in the background, without taking focus, with the number of updates
/// on its Dock icon. (macOS only allows notifications from apps signed with a paid developer account.)
enum DailyCheck {
    static let argument = "--check-updates"

    private static var label: String {
        Config.isTesting ? "com.lukegarito.smoothplugins.updatecheck.test" : "com.lukegarito.smoothplugins.updatecheck"
    }

    private static var plistURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appending(path: "Library/LaunchAgents/\(label).plist")
    }

    private static var testEnvironment: [String: String] {
        ProcessInfo.processInfo.environment.filter { $0.key.hasPrefix("SMOOTH_") }
    }

    /// An app opened straight from a download runs from a temporary copy that macOS deletes later.
    static var appIsInTemporaryLocation: Bool {
        Bundle.main.bundlePath.contains("/AppTranslocation/")
    }

    /// Writes (or refreshes, if the app moved) the agent and loads it.
    static func schedule() throws {
        guard let executable = Bundle.main.executablePath else { return }
        var job: [String: Any] = [
            "Label": label,
            "ProgramArguments": [executable, argument],
            "StartInterval": 86_400,
            "RunAtLoad": false,
            "ProcessType": "Background",
        ]
        if Config.isTesting {
            job["EnvironmentVariables"] = testEnvironment
        }
        let data = try PropertyListSerialization.data(fromPropertyList: job, format: .xml, options: 0)
        try FileManager.default.createDirectory(at: plistURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        if (try? Data(contentsOf: plistURL)) == data, isLoaded {
            return
        }
        unload()
        try data.write(to: plistURL, options: .atomic)
        try Shell.run("/bin/launchctl", ["bootstrap", "gui/\(getuid())", plistURL.path])
    }

    static func unschedule() {
        unload()
        try? FileManager.default.removeItem(at: plistURL)
    }

    private static var isLoaded: Bool {
        (try? Shell.run("/bin/launchctl", ["print", "gui/\(getuid())/\(label)"])) != nil
    }

    private static func unload() {
        try? Shell.run("/bin/launchctl", ["bootout", "gui/\(getuid())/\(label)"])
    }

    /// Runs when launchd starts the app with --check-updates: no window, no Dock icon.
    @MainActor
    static func runHeadless() {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        Task { @MainActor in
            await checkAndAnnounce()
            exit(0)
        }
        app.run()
    }

    @MainActor
    static func checkAndAnnounce() async {
        let manifest: ReleaseManifest
        do {
            manifest = try await UpdateChecker.latestRelease()
        } catch {
            debugLog("check failed: \(error.localizedDescription)")
            return
        }
        var names = manifest.plugins.map(PluginRow.init)
            .filter { $0.status == .updateAvailable }
            .map { "\($0.plugin.name) \($0.plugin.version)" }
        if Version.isNewer(manifest.app.version, than: Config.appVersion) {
            names.append("app \(manifest.app.version)")
        }
        let key = names.joined(separator: ",")
        debugLog("updates found: \(key.isEmpty ? "none" : key)")
        let defaults = UserDefaults.standard
        guard !names.isEmpty, key != defaults.string(forKey: "lastAnnouncedUpdates") else { return }

        let me = ProcessInfo.processInfo.processIdentifier
        let windowOpen = NSRunningApplication.runningApplications(withBundleIdentifier: Bundle.main.bundleIdentifier ?? "")
            .contains { $0.processIdentifier != me }
        if windowOpen {
            debugLog("the app is already open")
            return
        }

        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = false
        configuration.addsToRecentItems = false
        configuration.createsNewApplicationInstance = true
        if Config.isTesting {
            configuration.environment = testEnvironment
        }
        do {
            _ = try await NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, configuration: configuration)
            defaults.set(key, forKey: "lastAnnouncedUpdates")
            debugLog("opened the app in the background")
        } catch {
            debugLog("could not open the app: \(error.localizedDescription)")
        }
    }

    /// Prints to standard error, only when testing with the SMOOTH_* variables.
    static func debugLog(_ message: String) {
        guard Config.isTesting else { return }
        FileHandle.standardError.write(Data("[daily check] \(message)\n".utf8))
    }

    /// "A", "A and B", "A, B and 3 more".
    static func sentence(_ names: [String]) -> String {
        switch names.count {
        case 0: ""
        case 1: names[0]
        case 2: "\(names[0]) and \(names[1])"
        case 3: "\(names[0]), \(names[1]) and \(names[2])"
        default: "\(names[0]), \(names[1]) and \(names.count - 2) more"
        }
    }
}
