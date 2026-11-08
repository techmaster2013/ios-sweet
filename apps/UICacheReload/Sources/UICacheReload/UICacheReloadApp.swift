import SwiftUI

/// One binary, two modes: `--boot` (a launchd job at startup: register apps
/// and exit, no UI) or the normal app, which reloads on launch.
@main
struct Entry {
    static func main() {
        if CommandLine.arguments.contains("--boot") {
            UICache.runBoot()
            exit(0)
        }
        UICacheReloadApp.main()
    }
}

struct UICacheReloadApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
