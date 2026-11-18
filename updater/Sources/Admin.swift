import Foundation

/// Changes to /Library/Audio/Plug-Ins/Components (plug-ins installed for all users),
/// through the standard macOS administrator password prompt.
@MainActor
enum Admin {
    struct Cancelled: LocalizedError {
        var errorDescription: String? { "the administrator password was not given" }
    }

    static func replace(_ target: URL, with bundle: URL) throws {
        let t = Shell.quote(target.path)
        try runAsAdmin("/bin/rm -rf \(t) && /usr/bin/ditto \(Shell.quote(bundle.path)) \(t) && /usr/sbin/chown -R root:wheel \(t)")
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
