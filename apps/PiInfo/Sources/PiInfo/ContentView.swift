import SwiftUI
import UIKit

/// System facts, read the same way on a real iPhone/iPad and on the Pi 5 port.
@MainActor
struct SystemInfo {
    static func sysctlString(_ name: String) -> String? {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: size)
        guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { return nil }
        return String(decoding: buf.prefix(while: { $0 != 0 }), as: UTF8.self)
    }

    static var uname: (machine: String, release: String, version: String) {
        var u = utsname()
        Darwin.uname(&u)
        func field<T>(_ v: T) -> String {
            withUnsafeBytes(of: v) { raw in
                String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
            }
        }
        return (field(u.machine), field(u.release), field(u.version))
    }

    /// The ipados-rpi5 kernel is built on GitHub Actions ("runner:xnu-..."),
    /// Apple's say "root:xnu-...~N/RELEASE_ARM64_...".
    static var isRaspberryPi: Bool {
        uname.version.contains("runner:xnu")
    }

    static var deviceTitle: String {
        isRaspberryPi ? "Raspberry Pi 5" : UIDevice.current.model
    }

    static var osBuild: String { sysctlString("kern.osversion") ?? "?" }
    static var hwModel: String { sysctlString("hw.model") ?? "?" }

    static func bytes(_ n: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(n), countStyle: .memory)
    }

    static func diskBytes(_ n: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: n, countStyle: .file)
    }

    static var storage: (total: Int64, free: Int64)? {
        guard let a = try? FileManager.default.attributesOfFileSystem(forPath: NSHomeDirectory()),
              let total = (a[.systemSize] as? NSNumber)?.int64Value,
              let free = (a[.systemFreeSize] as? NSNumber)?.int64Value else { return nil }
        return (total, free)
    }

    static func duration(_ t: TimeInterval) -> String {
        let s = Int(t)
        let d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60
        return d > 0 ? String(format: "%dd %02d:%02d:%02d", d, h, m, sec)
                     : String(format: "%02d:%02d:%02d", h, m, sec)
    }
}

struct InfoRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
            Spacer(minLength: 16)
            Text(value)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
                .textSelection(.enabled)
        }
    }
}

struct ContentView: View {
    private let u = SystemInfo.uname
    private let p = ProcessInfo.processInfo

    var body: some View {
        NavigationStack {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                List {
                    Section {
                        VStack(alignment: .leading, spacing: 6) {
                            Image(systemName: SystemInfo.isRaspberryPi ? "cpu" : "iphone")
                                .font(.system(size: 44))
                                .foregroundStyle(SystemInfo.isRaspberryPi ? .pink : .blue)
                            Text(SystemInfo.deviceTitle)
                                .font(.largeTitle.bold())
                            Text(SystemInfo.isRaspberryPi
                                 ? "iPadOS on a Raspberry Pi 5 (ipados-rpi5)"
                                 : "\(UIDevice.current.systemName) on Apple hardware")
                                .foregroundStyle(.secondary)
                        }
                        .padding(.vertical, 8)
                    }

                    Section("System") {
                        InfoRow(label: "Name", value: UIDevice.current.name)
                        InfoRow(label: UIDevice.current.systemName,
                                value: "\(UIDevice.current.systemVersion) (\(SystemInfo.osBuild))")
                        if SystemInfo.isRaspberryPi {
                            // iPadOS needs a real iPad identity to run; show it, but
                            // say plainly what the hardware is.
                            InfoRow(label: "Hardware", value: "Raspberry Pi 5 (BCM2712, 4× Cortex-A76)")
                            InfoRow(label: "iPadOS identity", value: "\(u.machine) (\(SystemInfo.hwModel))")
                        } else {
                            InfoRow(label: "Identifier", value: u.machine)
                            InfoRow(label: "Board", value: SystemInfo.hwModel)
                        }
                        InfoRow(label: "Kernel", value: "Darwin \(u.release)")
                        InfoRow(label: "Uptime", value: SystemInfo.duration(p.systemUptime))
                    }

                    Section("Hardware") {
                        InfoRow(label: "CPU cores",
                                value: "\(p.activeProcessorCount) active of \(p.processorCount)")
                        InfoRow(label: "Memory", value: SystemInfo.bytes(p.physicalMemory))
                        if let s = SystemInfo.storage {
                            InfoRow(label: "Storage",
                                    value: "\(SystemInfo.diskBytes(s.free)) free of \(SystemInfo.diskBytes(s.total))")
                        }
                        InfoRow(label: "Screen", value: screenText)
                        InfoRow(label: "Thermal state", value: thermalText)
                    }

                    Section("Time") {
                        InfoRow(label: "Now", value: context.date.formatted(date: .abbreviated, time: .standard))
                        InfoRow(label: "Time zone", value: TimeZone.current.identifier)
                    }

                    Section("Kernel version") {
                        Text(u.version)
                            .font(.footnote.monospaced())
                            .textSelection(.enabled)
                    }
                }
            }
            .navigationTitle("PiInfo")
        }
    }

    private var screenText: String {
        let s = UIScreen.main
        let n = s.nativeBounds.size
        return "\(Int(n.width))×\(Int(n.height)) px @\(String(format: "%.0f", s.scale))x"
    }

    private var thermalText: String {
        switch p.thermalState {
        case .nominal: return "Nominal"
        case .fair: return "Fair"
        case .serious: return "Serious"
        case .critical: return "Critical"
        @unknown default: return "?"
        }
    }
}
