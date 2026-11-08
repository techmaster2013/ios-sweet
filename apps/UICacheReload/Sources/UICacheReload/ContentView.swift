import SwiftUI

struct ContentView: View {
    @State private var results: [UICache.Result] = []
    @State private var running = false
    @State private var lastRun: Date?

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button {
                        reload()
                    } label: {
                        HStack {
                            Text(running ? "Reloading…" : "Reload uicache")
                                .font(.headline)
                            Spacer()
                            if running { ProgressView() }
                        }
                    }
                    .disabled(running)
                } footer: {
                    if let lastRun {
                        Text("Last reload \(lastRun.formatted(date: .omitted, time: .standard)) · \(results.count) app(s)")
                    }
                }

                if !results.isEmpty {
                    Section("Registered apps") {
                        ForEach(results) { r in
                            HStack {
                                Image(systemName: r.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                                    .foregroundStyle(r.ok ? .green : .red)
                                VStack(alignment: .leading) {
                                    Text(r.name)
                                    Text(r.bundleID).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle("uicache reload")
        }
        .onAppear { reload() }
    }

    private func reload() {
        running = true
        Task {
            let r = await Task.detached(priority: .userInitiated) { UICache.reloadAll() }.value
            results = r
            lastRun = Date()
            running = false
        }
    }
}
