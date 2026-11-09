import Foundation

/// Registers apps with LaunchServices the way jailbreak `uicache` does:
/// read each bundle's Info.plist, add the registration keys installd would
/// add, and hand the dictionary to -[LSApplicationWorkspace
/// registerApplicationDictionary:]. Apple's own apps are left alone.
enum UICache {
    struct Result: Identifiable, Sendable {
        let id = UUID()
        let name: String
        let bundleID: String
        let ok: Bool
    }

    static let appDirectories = ["/Applications", "/var/jb/Applications"]
    static let useContainers = false

    /// One line to the console log. /dev/console is reopened for every line:
    /// the descriptor launchd hands an early-boot job is revoked when the user
    /// session starts (v6: FileHandle threw on it; v7: writes vanished). Also
    /// to stdout, for runs from a shell.
    static func log(_ s: String) {
        let bytes = Array("uicache reload: \(s)\n".utf8)
        bytes.withUnsafeBufferPointer { buf in
            let fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK)
            if fd >= 0 {
                _ = Darwin.write(fd, buf.baseAddress, buf.count)
                close(fd)
            }
            if isatty(STDOUT_FILENO) != 0 {
                _ = Darwin.write(STDOUT_FILENO, buf.baseAddress, buf.count)
            }
        }
    }

    // Set once, then only read (LSApplicationWorkspace is thread-safe).
    nonisolated(unsafe) private static let workspace: NSObject? = {
        dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW)
        guard let cls = NSClassFromString("LSApplicationWorkspace") as? NSObject.Type else { return nil }
        let sel = NSSelectorFromString("defaultWorkspace")
        guard cls.responds(to: sel) else { return nil }
        return cls.perform(sel)?.takeUnretainedValue() as? NSObject
    }()

    /// Data container for a system-style app (as uicache: MCMAppDataContainer).
    private static func dataContainer(for bundleID: String) -> String? {
        dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW)
        guard let cls: AnyClass = NSClassFromString("MCMAppDataContainer") else { return nil }
        let sel = NSSelectorFromString("containerWithIdentifier:createIfNecessary:existed:error:")
        guard let method = class_getClassMethod(cls, sel) else { return nil }
        typealias Fn = @convention(c) (AnyClass, Selector, NSString, Bool, UnsafeMutablePointer<ObjCBool>?,
                                       UnsafeMutablePointer<NSError?>?) -> Unmanaged<NSObject>?
        let fn = unsafeBitCast(method_getImplementation(method), to: Fn.self)
        var existed: ObjCBool = false
        var error: NSError?
        guard let container = fn(cls, sel, bundleID as NSString, true, &existed, &error)?.takeUnretainedValue(),
              let url = container.value(forKey: "url") as? URL else { return nil }
        return url.path
    }

    static func appBundles() -> [String] {
        var out: [String] = []
        for dir in appDirectories {
            guard let items = try? FileManager.default.contentsOfDirectory(atPath: dir) else { continue }
            for item in items.sorted() where item.hasSuffix(".app") {
                out.append("\(dir)/\(item)")
            }
        }
        return out
    }

    static func register(bundle path: String) -> Result? {
        guard let info = NSDictionary(contentsOfFile: "\(path)/Info.plist") as? [String: Any],
              let bundleID = info["CFBundleIdentifier"] as? String else {
            log("cannot read \(path)/Info.plist")
            return nil
        }
        let name = (info["CFBundleDisplayName"] as? String) ?? (info["CFBundleName"] as? String) ?? bundleID
        if bundleID.hasPrefix("com.apple.") {
            return nil
        }
        var dict = info
        dict["ApplicationType"] = "System"
        dict["CFBundleIdentifier"] = bundleID
        dict["CodeInfoIdentifier"] = bundleID
        dict["CompatibilityState"] = 0
        dict["IsDeletable"] = false
        dict["Path"] = path
        dict["SignerOrganization"] = "Apple Inc."
        dict["SignerIdentity"] = "Apple iPhone OS Application Signing"
        dict["IsAdHocSigned"] = true
        dict["HasMIDBasedSINF"] = false
        dict["MissingSINF"] = false
        dict["FamilyID"] = 0
        dict["IsOnDemandInstallCapable"] = false
        // Built-in apps in /Applications run without a data container (HOME is
        // /var/mobile); creating one needs containermanagerd, which may not be up yet.
        if useContainers, let home = dataContainer(for: bundleID) {
            dict["Container"] = home
            dict["EnvironmentVariables"] = [
                "CFFIXED_USER_HOME": home,
                "HOME": home,
                "TMPDIR": "\(home)/tmp",
            ]
        }
        var ok = false
        let sel = NSSelectorFromString("registerApplicationDictionary:")
        if let ws = workspace, ws.responds(to: sel) {
            typealias Fn = @convention(c) (NSObject, Selector, NSDictionary) -> Bool
            let fn = unsafeBitCast(ws.method(for: sel), to: Fn.self)
            ok = fn(ws, sel, dict as NSDictionary)
        }
        return Result(name: name, bundleID: bundleID, ok: ok)
    }

    static func reloadAll() -> [Result] {
        appBundles().compactMap { register(bundle: $0) }
    }

    /// How LaunchServices sees an app after registration (LSApplicationProxy).
    static func describe(_ bundleID: String) -> String {
        guard let cls = NSClassFromString("LSApplicationProxy") as? NSObject.Type else { return "no LSApplicationProxy" }
        let sel = NSSelectorFromString("applicationProxyForIdentifier:")
        guard cls.responds(to: sel),
              let proxy = cls.perform(sel, with: bundleID)?.takeUnretainedValue() as? NSObject else { return "no proxy" }
        func val(_ key: String) -> String {
            guard proxy.responds(to: NSSelectorFromString(key)) else { return "?" }
            return String(describing: proxy.value(forKey: key) ?? "nil")
        }
        var appState = "?"
        if proxy.responds(to: NSSelectorFromString("appState")),
           let st = proxy.value(forKey: "appState") as? NSObject {
            appState = "installed=\(st.value(forKey: "isInstalled") ?? "?") valid=\(st.value(forKey: "isValid") ?? "?")"
        }
        return "type=\(val("applicationType")) installed=\(val("isInstalled")) \(appState) " +
            "launchProhibited=\(val("isLaunchProhibited")) tags=\(val("appTags")) path=\(val("bundleURL"))"
    }

    /// Restart SpringBoard so it rebuilds its icon list (a "respring").
    static func respring() {
        var name = [Int32](arrayLiteral: CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0)
        var size = 0
        guard sysctl(&name, 4, nil, &size, nil, 0) == 0, size > 0 else { return }
        let count = size / MemoryLayout<kinfo_proc>.stride
        var procs = [kinfo_proc](repeating: kinfo_proc(), count: count + 16)
        size = procs.count * MemoryLayout<kinfo_proc>.stride
        guard sysctl(&name, 4, &procs, &size, nil, 0) == 0 else { return }
        for i in 0..<(size / MemoryLayout<kinfo_proc>.stride) {
            var p = procs[i]
            let comm = withUnsafeBytes(of: &p.kp_proc.p_comm) { raw in
                String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
            }
            if comm == "SpringBoard" {
                log("respring: SIGTERM SpringBoard (pid \(p.kp_proc.p_pid))")
                kill(p.kp_proc.p_pid, SIGTERM)
            }
        }
    }

    /// launchd boot job: wait for the system (lsd, SpringBoard) to be up, register
    /// everything, retry a few times, log each step unbuffered, exit.
    static func runBoot() {
        log("boot job started, waiting 20 s for the system to come up")
        Thread.sleep(forTimeInterval: 20)
        log("apps found: \(appBundles().joined(separator: ", "))")
        for attempt in 1...5 {
            log("attempt \(attempt): LSApplicationWorkspace \(workspace == nil ? "MISSING" : "ok")")
            let results = reloadAll()
            for r in results {
                log("\(r.ok ? "registered" : "FAILED") \(r.name) (\(r.bundleID))")
                log("  -> \(describe(r.bundleID))")
            }
            if !results.isEmpty && results.allSatisfy(\.ok) {
                // No respring: with SBShowNonDefaultSystemApps set, SpringBoard
                // picks newly registered apps up live (the Respring button stays).
                log("done")
                return
            }
            Thread.sleep(forTimeInterval: 20)
        }
        log("gave up after 5 attempts")
    }
}
