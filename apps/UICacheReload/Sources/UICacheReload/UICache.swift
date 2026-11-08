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
              let bundleID = info["CFBundleIdentifier"] as? String else { return nil }
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
        if let home = dataContainer(for: bundleID) {
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

    /// launchd boot job: register everything, log to stdout (the console), exit.
    static func runBoot() {
        let results = reloadAll()
        print("uicache reload: \(workspace == nil ? "NO LSApplicationWorkspace" : "LSApplicationWorkspace ok"), \(results.count) app(s)")
        for r in results {
            print("uicache reload: \(r.ok ? "registered" : "FAILED") \(r.name) (\(r.bundleID))")
        }
    }
}
