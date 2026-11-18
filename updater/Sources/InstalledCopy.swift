import Foundation

/// A plug-in found in one of the Components folders.
struct InstalledCopy: Sendable, Hashable {
    enum Scope: Sendable, Hashable {
        case me
        case allUsers
    }

    let url: URL
    let version: String
    let scope: Scope
}
