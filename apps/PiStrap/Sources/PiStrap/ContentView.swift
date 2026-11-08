import SwiftUI

/// What PiStrap can install. The install steps are a preview until the
/// bootstrap packages ship in the image (see apps/PiStrap/README in the repo).
struct StrapOptions {
    var sileo = true
    var zebra = false
    var trollStoreLite = true
    var cydia = false          // needs the rootful ("classic") layout
    var ellekit = true

    var rootful: Bool { cydia }
}

enum StrapState: Equatable {
    case idle, running, done
}

@MainActor
final class Strapper: ObservableObject {
    @Published var state: StrapState = .idle
    @Published var log: [String] = []
    @Published var options = StrapOptions()

    /// The real steps, in order. In this preview each one is only announced.
    func steps() -> [String] {
        var s = [
            options.rootful ? "Preparing the classic (rootful) layout in /" : "Creating /var/jb (rootless)",
            "Extracting the Procursus bootstrap",
            "Setting up apt sources",
        ]
        if options.ellekit { s.append("Installing ElleKit (tweak injection)") }
        if options.sileo { s.append("Installing Sileo") }
        if options.zebra { s.append("Installing Zebra") }
        if options.cydia { s.append("Installing Cydia") }
        if options.trollStoreLite { s.append("Installing TrollStore Lite") }
        s.append("Registering apps (uicache)")
        return s
    }

    func run() {
        guard state != .running else { return }
        state = .running
        log = ["PiStrap preview: nothing is changed on this device."]
        Task {
            for step in steps() {
                log.append("• \(step)…")
                try? await Task.sleep(nanoseconds: 450_000_000)
            }
            log.append("Done (preview). The real install arrives with network + bootstrap packages.")
            state = .done
        }
    }
}

struct ContentView: View {
    @StateObject private var strapper = Strapper()
    @State private var showOptions = false
    @State private var showManagers = false

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.10, green: 0.05, blue: 0.20),
                                    Color(red: 0.55, green: 0.10, blue: 0.30),
                                    Color(red: 0.95, green: 0.35, blue: 0.40)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
                .ignoresSafeArea()

            VStack(spacing: 22) {
                Spacer(minLength: 20)

                VStack(spacing: 6) {
                    Image(systemName: "bolt.horizontal.circle.fill")
                        .font(.system(size: 64, weight: .semibold))
                        .foregroundStyle(.white.opacity(0.9))
                    Text("PiStrap")
                        .font(.system(size: 44, weight: .heavy, design: .rounded))
                        .foregroundStyle(.white)
                    Text("iPadOS \(UIDevice.current.systemVersion) · Raspberry Pi 5")
                        .font(.subheadline.weight(.medium))
                        .foregroundStyle(.white.opacity(0.75))
                }

                logBox
                    .frame(maxWidth: 520)

                Button {
                    showManagers = true
                } label: {
                    HStack(spacing: 10) {
                        if strapper.state == .running {
                            ProgressView().tint(.white)
                        } else {
                            Image(systemName: strapper.state == .done ? "checkmark.circle.fill" : "lock.open.fill")
                        }
                        Text(buttonTitle)
                            .font(.title3.bold())
                    }
                    .frame(maxWidth: 520, minHeight: 56)
                    .foregroundStyle(.white)
                    .background(.white.opacity(0.18), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .stroke(.white.opacity(0.35), lineWidth: 1))
                }
                .disabled(strapper.state == .running)

                Button {
                    showOptions = true
                } label: {
                    Label("Options", systemImage: "gearshape.fill")
                        .font(.body.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.85))
                }

                Spacer(minLength: 20)
                Text("ipados-rpi5 · preview build")
                    .font(.footnote)
                    .foregroundStyle(.white.opacity(0.5))
            }
            .padding(.horizontal, 24)
        }
        .sheet(isPresented: $showOptions) {
            OptionsView(options: $strapper.options)
        }
        .sheet(isPresented: $showManagers) {
            ManagersView(options: $strapper.options) {
                showManagers = false
                strapper.run()
            }
        }
    }

    private var buttonTitle: String {
        switch strapper.state {
        case .idle: return "Jailbreak"
        case .running: return "Jailbreaking…"
        case .done: return "Jailbroken (preview)"
        }
    }

    private var logBox: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(Array(strapper.log.enumerated()), id: \.offset) { i, line in
                        Text(line)
                            .font(.footnote.monospaced())
                            .foregroundStyle(.white.opacity(0.9))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id(i)
                    }
                    if strapper.log.isEmpty {
                        Text("Ready.")
                            .font(.footnote.monospaced())
                            .foregroundStyle(.white.opacity(0.6))
                    }
                }
                .padding(14)
            }
            .frame(height: 200)
            .background(.black.opacity(0.25), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            .onChange(of: strapper.log.count) { n in
                withAnimation { proxy.scrollTo(n - 1, anchor: .bottom) }
            }
        }
    }
}

/// Shown when Jailbreak is pressed: pick what to install, then go.
struct ManagersView: View {
    @Binding var options: StrapOptions
    let start: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Sileo", isOn: $options.sileo)
                    Toggle("Zebra", isOn: $options.zebra)
                    Toggle("TrollStore Lite", isOn: $options.trollStoreLite)
                } header: {
                    Text("Package managers")
                } footer: {
                    Text("Sileo is the recommended package manager for iPadOS 16. TrollStore Lite installs IPA files.")
                }
                Section {
                    Toggle("Cydia (classic)", isOn: $options.cydia)
                } footer: {
                    Text("Cydia needs the old rootful layout in /. Most modern tweaks expect rootless, so leave this off unless you want Cydia.")
                }
                Section {
                    Button {
                        start()
                    } label: {
                        Text("Jailbreak")
                            .font(.headline)
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(!(options.sileo || options.zebra || options.trollStoreLite || options.cydia))
                }
            }
            .navigationTitle("Choose package managers")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }
}

struct OptionsView: View {
    @Binding var options: StrapOptions
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("ElleKit", isOn: $options.ellekit)
                } header: {
                    Text("Tweaks")
                } footer: {
                    Text("Tweak injection for rootless packages.")
                }
            }
            .navigationTitle("Options")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
