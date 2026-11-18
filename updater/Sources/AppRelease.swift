import Foundation

/// The latest version of this app. Releases before app 1.1.0 carried no checksum.
struct AppRelease: Codable, Sendable, Equatable {
    let version: String
    let url: URL
    let sha256: String?
    let size: Int?
}
