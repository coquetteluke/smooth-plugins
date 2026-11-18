import Foundation

/// The latest version of this app.
struct AppRelease: Codable, Sendable, Equatable {
    let version: String
    let url: URL
}
