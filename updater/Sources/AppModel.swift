import AppKit

@MainActor
final class AppModel: ObservableObject {
    enum Phase: Equatable {
        case checking
        case ready
        case failed(String)
    }

    @Published private(set) var rows: [PluginRow] = []
    @Published private(set) var pickedIDs: Set<String> = []
    @Published private(set) var phase: Phase = .checking
    @Published private(set) var busyMessage: String?
    @Published private(set) var resultMessage: String?
    @Published private(set) var appUpdate: AppRelease?
    @Published private(set) var logicIsOpen = false
    @Published var dailyCheck: Bool {
        didSet {
            UserDefaults.standard.set(dailyCheck, forKey: "dailyCheck")
            applyDailyCheck()
        }
    }

    init() {
        dailyCheck = UserDefaults.standard.object(forKey: "dailyCheck") as? Bool ?? true
    }

    var canScheduleDailyCheck: Bool { !DailyCheck.appIsInTemporaryLocation }

    var updateCount: Int { rows.filter { $0.status == .updateAvailable }.count }

    /// Whether a plug-in's checkbox is ticked.
    subscript(picked id: String) -> Bool {
        get { pickedIDs.contains(id) }
        set {
            if newValue {
                pickedIDs.insert(id)
            } else {
                pickedIDs.remove(id)
            }
        }
    }

    var toInstall: [PluginRow] {
        rows.filter { pickedIDs.contains($0.id) && $0.status != .upToDate }
    }

    var toRemove: [PluginRow] {
        rows.filter { pickedIDs.contains($0.id) && !$0.copies.isEmpty }
    }

    var isBusy: Bool { busyMessage != nil }

    func start() async {
        applyDailyCheck()
        await refresh()
    }

    func refresh() async {
        phase = .checking
        resultMessage = nil
        logicIsOpen = NSWorkspace.shared.runningApplications.contains { $0.bundleIdentifier == Config.logicBundleID }
        do {
            let manifest = try await UpdateChecker.latestRelease()
            appUpdate = Version.isNewer(manifest.app.version, than: Config.appVersion) ? manifest.app : nil
            rows = manifest.plugins.map(PluginRow.init)
            pickDefaults()
            updateDockBadge()
            phase = .ready
        } catch {
            phase = .failed(error.localizedDescription)
        }
    }

    /// Updates are picked; on a Mac with none of the plug-ins, all of them are.
    func pickDefaults() {
        let hasAny = rows.contains { $0.status != .notInstalled }
        pickedIDs = Set(rows.filter { hasAny ? $0.status == .updateAvailable : true }.map(\.id))
    }

    func pick(_ choose: (PluginRow) -> Bool) {
        pickedIDs = Set(rows.filter(choose).map(\.id))
    }

    func installPicked() async {
        let rows = toInstall
        guard !rows.isEmpty else { return }
        var done: [String] = []
        var failed: [String] = []
        for (index, row) in rows.enumerated() {
            busyMessage = "\(row.status == .updateAvailable ? "Updating" : "Installing") \(row.plugin.name) (\(index + 1) of \(rows.count))"
            do {
                try await PluginInstaller.install(row.plugin, replacing: row.copies)
                done.append(row.plugin.name)
            } catch {
                failed.append("\(row.plugin.name): \(error.localizedDescription)")
            }
        }
        let allUpdates = rows.allSatisfy { $0.status == .updateAvailable }
        finish(done: done, verb: allUpdates ? "Updated" : "Installed", failed: failed)
    }

    func removePicked() async {
        let rows = toRemove
        guard !rows.isEmpty else { return }
        var done: [String] = []
        var failed: [String] = []
        for row in rows {
            busyMessage = "Removing \(row.plugin.name)"
            do {
                try await PluginInstaller.remove(row.copies)
                done.append(row.plugin.name)
            } catch {
                failed.append("\(row.plugin.name): \(error.localizedDescription)")
            }
        }
        finish(done: done, verb: "Removed", failed: failed)
    }

    func openReleasesPage() {
        NSWorkspace.shared.open(Config.releasesPage)
    }

    private func finish(done: [String], verb: String, failed: [String]) {
        PluginInstaller.refreshHosts()
        busyMessage = nil
        rows = rows.map { PluginRow(plugin: $0.plugin) }
        pickDefaults()
        updateDockBadge()
        var parts: [String] = []
        if !done.isEmpty {
            parts.append("\(verb) \(DailyCheck.sentence(done)).")
        }
        if !failed.isEmpty {
            parts.append("Not done: " + failed.joined(separator: "; ") + ".")
        }
        if !done.isEmpty && logicIsOpen {
            parts.append("Restart Logic Pro to load the changes.")
        }
        resultMessage = parts.joined(separator: " ")
    }

    /// The number of updates on the app's Dock icon.
    private func updateDockBadge() {
        let count = updateCount + (appUpdate == nil ? 0 : 1)
        NSApplication.shared.dockTile.badgeLabel = count > 0 ? "\(count)" : nil
    }

    private func applyDailyCheck() {
        guard canScheduleDailyCheck else { return }
        if dailyCheck {
            try? DailyCheck.schedule()
        } else {
            DailyCheck.unschedule()
        }
    }
}
