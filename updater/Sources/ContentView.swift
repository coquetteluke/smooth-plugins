import SwiftUI

struct ContentView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            HeaderView(model: model)
            if let appUpdate = model.appUpdate {
                NoticeView(
                    symbol: "arrow.down.app",
                    text: "A new version of this app (\(appUpdate.version)) is out.",
                    buttonTitle: "Download",
                    action: model.openReleasesPage
                )
            }
            if model.logicIsOpen {
                NoticeView(
                    symbol: "info.circle",
                    text: "Logic Pro is open. Restart it after installing to load the changes."
                )
            }
            PluginListView(model: model)
            Divider()
            FooterView(model: model)
        }
        .task {
            await model.start()
        }
    }
}
