import SwiftUI

struct SmoothPluginsApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        Window("Smooth Plugins", id: "main") {
            ContentView(model: model)
                .frame(minWidth: 600, minHeight: 520)
        }
        .defaultSize(width: 700, height: 740)
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}
