import Foundation

/// Reads the latest release's manifest from GitHub.
enum UpdateChecker {
    enum Failure: LocalizedError {
        case http(Int)
        case newerFormat

        var errorDescription: String? {
            switch self {
            case .http(let code): "GitHub answered with error \(code)"
            case .newerFormat: "this version of the app is too old to read the latest release; download the new app"
            }
        }
    }

    @concurrent
    static func latestRelease() async throws -> ReleaseManifest {
        let request = URLRequest(url: Config.manifestURL, cachePolicy: .reloadIgnoringLocalAndRemoteCacheData, timeoutInterval: 20)
        let (data, response) = try await URLSession.shared.data(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw Failure.http(http.statusCode)
        }
        let manifest = try JSONDecoder().decode(ReleaseManifest.self, from: data)
        guard manifest.formatVersion == 1 else {
            throw Failure.newerFormat
        }
        return manifest
    }
}
