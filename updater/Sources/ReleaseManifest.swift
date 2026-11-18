import Foundation

/// manifest.json, published with every release: what the latest versions are and where to get them.
struct ReleaseManifest: Codable, Sendable {
    let formatVersion: Int
    let release: String
    let app: AppRelease
    let plugins: [PluginRelease]
}
