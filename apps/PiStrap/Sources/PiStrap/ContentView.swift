import SwiftUI

import Darwin

/// What PiStrap can install. Packages come from PiStrap.app/Packages or
/// /var/mobile/Library/PiStrap (see apps/PiStrap/README.md in the repo).
struct StrapOptions {
    var sileo = true
    var zebra = false
    var trollStoreLite = true
    var cydia = false          // needs the rootful ("classic") layout: not offered yet
    var ellekit = true
}

enum StrapState: Equatable {
    case idle, running, done, failed
}

/// One package file PiStrap looks for, by file-name prefix.
struct PackageSpec {
    let title: String
    let prefixes: [String]
}

@MainActor
final class Strapper: ObservableObject {
    @Published var state: StrapState = .idle
    @Published var log: [String] = []
    @Published var options = StrapOptions()

    nonisolated static let userDir = "/var/mobile/Library/PiStrap"
    nonisolated static var appPackages: String { Bundle.main.bundlePath + "/Packages" }
    nonisolated static var helper: String { Bundle.main.bundlePath + "/pistrap-helper" }

    /// Package files available on the device (bundle first, then the user folder).
    static func packageFiles() -> [String] {
        var out: [String] = []
        for dir in [appPackages, userDir] {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []
            out += names.sorted().map { dir + "/" + $0 }
        }
        return out
    }

    static func find(_ spec: PackageSpec, in files: [String]) -> String? {
        files.first { path in
            let name = (path as NSString).lastPathComponent.lowercased()
            return spec.prefixes.contains { name.hasPrefix($0) }
        }
    }

    func selectedDebs() -> [PackageSpec] {
        var s: [PackageSpec] = []
        if options.ellekit { s.append(PackageSpec(title: "ElleKit", prefixes: ["ellekit"])) }
        if options.sileo { s.append(PackageSpec(title: "Sileo", prefixes: ["org.coolstar.sileo", "sileo"])) }
        if options.zebra { s.append(PackageSpec(title: "Zebra", prefixes: ["xyz.willy.zebra", "zebra"])) }
        if options.trollStoreLite {
            // TrollStore Lite depends on ldid, which the Procursus bootstrap
            // lacks (and ldid on libplist3): install them in the same dpkg run.
            s.append(PackageSpec(title: "libplist3", prefixes: ["libplist3_"]))
            s.append(PackageSpec(title: "ldid", prefixes: ["ldid_"]))
            s.append(PackageSpec(title: "TrollStore Lite", prefixes: ["com.opa334.trollstorelite", "trollstorelite", "trollstore-lite"]))
        }
        return s
    }

    /// Run the root helper; each output line goes to the log. Returns the exit status.
    nonisolated static func runHelper(_ args: [String], line: @escaping @Sendable (String) -> Void) -> Int32 {
        var fds: [Int32] = [0, 0]
        guard pipe(&fds) == 0 else { line("error: pipe failed"); return -1 }
        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1)
        posix_spawn_file_actions_adddup2(&actions, fds[1], 2)
        posix_spawn_file_actions_addclose(&actions, fds[0])
        let argv = ([helper] + args).map { strdup($0) } + [nil]
        var pid: pid_t = 0
        let rc = posix_spawn(&pid, helper, &actions, nil, argv, environ)
        posix_spawn_file_actions_destroy(&actions)
        argv.forEach { free($0) }
        close(fds[1])
        guard rc == 0 else {
            close(fds[0])
            line("error: cannot start the helper (\(String(cString: strerror(rc))))")
            return -1
        }
        let fh = FileHandle(fileDescriptor: fds[0], closeOnDealloc: true)
        var buffer = Data()
        while true {
            let chunk = fh.availableData
            if chunk.isEmpty { break }
            buffer.append(chunk)
            while let nl = buffer.firstIndex(of: 10) {
                line(String(decoding: buffer[buffer.startIndex..<nl], as: UTF8.self))
                buffer.removeSubrange(buffer.startIndex...nl)
            }
        }
        if !buffer.isEmpty { line(String(decoding: buffer, as: UTF8.self)) }
        var status: Int32 = 0
        waitpid(pid, &status, 0)
        return (status & 0x7f) == 0 ? (status >> 8) & 0xff : -1
    }

    func run() {
        guard state != .running else { return }
        state = .running
        log = []
        let files = Self.packageFiles()
        let bootstrap = Self.find(PackageSpec(title: "Procursus bootstrap", prefixes: ["bootstrap"]), in: files)
        let debs = selectedDebs().map { ($0.title, Self.find($0, in: files)) }
        let say: @Sendable (String) -> Void = { [weak self] s in
            Task { @MainActor in self?.log.append(s) }
        }
        let finish: @Sendable (Bool) -> Void = { [weak self] ok in
            Task { @MainActor in
                self?.log.append(ok ? "Done." : "Stopped: see the messages above.")
                self?.state = ok ? .done : .failed
            }
        }
        Task.detached {
            finish(Strapper.work(bootstrap: bootstrap, debs: debs, say: say))
        }
    }

    /// The install itself, off the main thread. Returns true on success.
    nonisolated static func work(bootstrap: String?, debs: [(String, String?)],
                                 say: @escaping @Sendable (String) -> Void) -> Bool {
        func step(_ title: String, _ args: [String]) -> Bool {
            say("• \(title)")
            return runHelper(args, line: { say("  " + $0) }) == 0
        }
        guard step("Checking the root helper", ["status"]) else { return false }
        guard let b = bootstrap else {
            say("✗ No Procursus bootstrap on this device (bootstrap-*.tar* in PiStrap.app/Packages or \(userDir))")
            return false
        }
        guard step("Installing the Procursus bootstrap into /var/jb", ["bootstrap", b]) else { return false }
        for (title, path) in debs where path == nil {
            say("✗ \(title): package not on this device yet, skipped")
        }
        let present = debs.compactMap { $0.1 }
        if !present.isEmpty {
            let names = debs.filter { $0.1 != nil }.map { $0.0 }.joined(separator: ", ")
            guard step("Installing " + names, ["install"] + present) else { return false }
        }
        return step("Registering apps (uicache)", ["uicache"])
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
                Text("ipados-rpi5 · rootless Procursus in /var/jb")
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
        case .done: return "Jailbroken"
        case .failed: return "Try again"
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
                        .disabled(true)
                } footer: {
                    Text("Cydia needs the old rootful layout in /. PiStrap installs rootless (/var/jb) for now, so Cydia is not offered yet.")
                }
                Section {
                    Button {
                        start()
                    } label: {
                        Text("Jailbreak")
                            .font(.headline)
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(!(options.sileo || options.zebra || options.trollStoreLite))
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
