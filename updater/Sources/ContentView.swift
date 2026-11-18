import SwiftUI

struct ContentView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            HeaderView(model: model)
            if let appUpdate = model.appUpdate {
                if model.appUpdateNeedsDownload || !AppUpdater.canUpdateInPlace || appUpdate.sha256 == nil {
                    NoticeView(
                        symbol: "arrow.down.app",
                        text: "Smooth Plugins \(appUpdate.version) is out.",
                        buttonTitle: "Download",
                        action: model.openReleasesPage
                    )
                } else {
                    NoticeView(
                        symbol: "arrow.down.app",
                        text: "Smooth Plugins \(appUpdate.version) is out.",
                        buttonTitle: "Update App",
                        action: { Task { await model.updateApp() } }
                    )
                    .disabled(model.isBusy)
                }
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
