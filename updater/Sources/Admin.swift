import Foundation

/// Changes to /Library/Audio/Plug-Ins/Components (plug-ins installed for all users),
/// through the standard macOS administrator password prompt.
@MainActor
enum Admin {
    struct Cancelled: LocalizedError {
        var errorDescription: String? { "the administrator password was not given" }
    }

    /// Installs the bundle `bundleName` from `zip` at `target`, as root. The root command first copies the zip
    /// into a folder only root can write, checks that copy's SHA-256 and the unpacked bundle's signature, and
    /// installs from there, so nothing running as the user can swap the files between the check and the copy.
    static func install(zip: URL, sha256: String, bundleName: String, at target: URL) throws {
        try runAsAdmin(installCommand(zip: zip, sha256: sha256, bundleName: bundleName, target: target))
    }

    /// The shell command `install` runs as root. Kept apart so it can be checked without a password.
    nonisolated static func installCommand(zip: URL, sha256: String, bundleName: String, target: URL, owner: String = "root:wheel") -> String {
        let t = Shell.quote(target.path)
        let unpacked = "\"$d/unpacked/\"" + Shell.quote(bundleName)
        return [
            "d=$(/usr/bin/mktemp -d /private/tmp/smoothplugins.XXXXXX)",
            "trap '/bin/rm -rf \"$d\"' EXIT",
            "/bin/cp \(Shell.quote(zip.path)) \"$d/download.zip\"",
            "[ \"$(/usr/bin/shasum -a 256 \"$d/download.zip\" | /usr/bin/cut -d ' ' -f 1)\" = \(Shell.quote(sha256.lowercased())) ]",
            "/usr/bin/ditto -x -k \"$d/download.zip\" \"$d/unpacked\"",
            "/usr/bin/codesign --verify --deep --strict \(unpacked)",
            "/bin/rm -rf \(t)",
            "/usr/bin/ditto \(unpacked) \(t)",
            "/usr/sbin/chown -R \(owner) \(t)",
        ].joined(separator: " && ")
    }

    static func remove(_ target: URL) throws {
        try runAsAdmin("/bin/rm -rf \(Shell.quote(target.path))")
    }

    /// The AppleScript that runs `command` as root. Kept apart so it can be checked without running it.
    nonisolated static func script(for command: String) -> String {
        let escaped = command
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "do shell script \"\(escaped)\" with administrator privileges"
    }

    private static func runAsAdmin(_ command: String) throws {
        var error: NSDictionary?
        guard let script = NSAppleScript(source: script(for: command)) else {
            throw Shell.Failure(tool: "AppleScript", output: "could not build the script")
        }
        script.executeAndReturnError(&error)
        if let error {
            if (error[NSAppleScript.errorNumber] as? Int) == -128 {
                throw Cancelled()
            }
            throw Shell.Failure(tool: "AppleScript", output: error[NSAppleScript.errorMessage] as? String ?? "")
        }
    }
}
