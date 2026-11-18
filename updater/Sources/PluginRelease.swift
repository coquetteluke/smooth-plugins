import Foundation

/// One plug-in in the latest release.
struct PluginRelease: Codable, Sendable, Identifiable, Equatable {
    let id: String
    let name: String
    let kind: String
    let description: String
    let version: String
    let bundleId: String
    let url: URL
    let sha256: String
    let size: Int
}
