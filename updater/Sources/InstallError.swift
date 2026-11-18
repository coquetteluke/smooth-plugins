import Foundation

enum InstallError: LocalizedError {
    case untrustedLink
    case checksumMismatch
    case unexpectedContents

    var errorDescription: String? {
        switch self {
        case .untrustedLink: "the download link is not from the Smooth Plugins releases"
        case .checksumMismatch: "the download was damaged (checksum mismatch)"
        case .unexpectedContents: "the download did not contain the expected plug-in"
        }
    }
}
