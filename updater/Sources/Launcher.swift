import Foundation

/// Starts the window, or, when launchd runs the daily check, checks for updates without one.
@main
enum Launcher {
    @MainActor
    static func main() {
        if CommandLine.arguments.contains(DailyCheck.argument) {
            DailyCheck.runHeadless()
        } else {
            SmoothPluginsApp.main()
        }
    }
}
