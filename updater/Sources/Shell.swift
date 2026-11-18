import Foundation

/// Runs a command-line tool and waits for it.
enum Shell {
    struct Failure: LocalizedError {
        let tool: String
        let output: String
        var errorDescription: String? {
            output.isEmpty ? "\(tool) failed" : "\(tool) failed: \(output)"
        }
    }

    static func run(_ tool: String, _ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(filePath: tool)
        process.arguments = arguments
        let errors = Pipe()
        process.standardError = errors
        process.standardOutput = FileHandle.nullDevice
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            let data = errors.fileHandleForReading.readDataToEndOfFile()
            let text = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            throw Failure(tool: URL(filePath: tool).lastPathComponent, output: text)
        }
    }

    /// Quotes a path for /bin/sh.
    static func quote(_ text: String) -> String {
        "'" + text.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
