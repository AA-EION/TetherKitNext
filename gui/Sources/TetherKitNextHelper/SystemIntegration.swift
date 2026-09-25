import Darwin
import Foundation
import TetherKitNextIPC

/// Where this daemon's own app bundle is, if it runs from one.
///
/// Under SMAppService the daemon executes in place from
/// `TetherKitNext.app/Contents/MacOS/tetherkitnext-helper` (the plist's
/// `BundleProgram`). Everything bundle-relative is derived from the kernel's
/// view of our executable path, never from anything a client sends.
enum OwnBundle {
    /// `…/TetherKitNext.app/Contents`, or nil when not running from an app bundle
    /// (e.g. a development LaunchDaemon install).
    static let contentsDirectory: URL? = {
        var size = UInt32(PATH_MAX)
        var buffer = [CChar](repeating: 0, count: Int(size))
        guard _NSGetExecutablePath(&buffer, &size) == 0,
              let resolved = realpath(buffer, nil) else { return nil }
        defer { free(resolved) }
        let executable = URL(fileURLWithPath: String(cString: resolved))
        let contents = executable.deletingLastPathComponent().deletingLastPathComponent()
        guard contents.lastPathComponent == "Contents",
              contents.deletingLastPathComponent().pathExtension == "app" else { return nil }
        return contents
    }()
}

/// Removes the pre-SMAppService installation (LaunchDaemon + files under
/// /Library/PrivilegedHelperTools) that upstream TetherKit 0.1.x builds installed
/// through AuthorizationExecuteWithPrivileges.
///
/// Doing it here, as root, on the new daemon's first start means an upgrade
/// needs no extra password prompt and leaves no stale root-owned binaries
/// behind. Only paths belonging to the legacy install are touched.
enum LegacyHelper {
    static func removeIfPresent() -> Bool {
        let fileManager = FileManager.default
        let legacy = HelperConstants.Legacy.self
        guard fileManager.fileExists(atPath: legacy.launchDaemonPlistPath)
                || fileManager.fileExists(atPath: legacy.executablePath) else {
            return false
        }

        // bootout sends SIGTERM; the legacy helper stops its session and
        // destroys its feth pair on the way out.
        _ = run("/bin/launchctl", ["bootout", "system/\(legacy.label)"])

        var paths = [legacy.launchDaemonPlistPath, legacy.executablePath]
        let tools = URL(fileURLWithPath: legacy.toolsDirectory)
        if let entries = try? fileManager.contentsOfDirectory(atPath: tools.path) {
            paths += entries
                .filter { $0.hasPrefix("libtetherkit") || $0.hasPrefix("libusb-") }
                .map { tools.appendingPathComponent($0).path }
        }
        for path in paths {
            try? fileManager.removeItem(atPath: path)
        }
        return true
    }

    private static func run(_ executable: String, _ arguments: [String]) -> Int32 {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.environment = ["PATH": "/usr/bin:/bin:/usr/sbin:/sbin"]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus
        } catch {
            return -1
        }
    }
}

/// Manages the `/usr/local/bin/tetherkitnext-cli` symlink.
///
/// Security notes (this runs as root):
///   * The link target is always the CLI inside *this daemon's own* bundle.
///   * /usr/local/bin may be user-writable (Homebrew on Intel chowns it), so
///     the directory itself must be a real directory, and an existing entry is
///     only replaced when it is a symlink that already points into a
///     TetherKitNext.app bundle or dangles. A regular file or a foreign symlink
///     (e.g. a Homebrew formula) is reported, never clobbered.
///   * symlink(2)/unlink(2) operate on the link itself and never follow it.
enum CommandLineToolLink {
    enum Failure: LocalizedError {
        case notInBundle
        case toolMissing(String)
        case directoryUnsafe(String)
        case occupied(String)
        case system(String, Int32)

        var errorDescription: String? {
            switch self {
            case .notInBundle: return L(.cliLinkNotInBundle)
            case .toolMissing(let path): return L(.cliLinkToolMissing, path)
            case .directoryUnsafe(let path): return L(.cliLinkDirectoryUnsafe, path)
            case .occupied(let detail): return L(.cliLinkOccupied, detail)
            case .system(let what, let code):
                return L(.cliLinkSystemError, what, String(cString: strerror(code)))
            }
        }
    }

    static func install() throws {
        guard let contents = OwnBundle.contentsDirectory else { throw Failure.notInBundle }
        let tool = contents.appendingPathComponent(HelperConstants.commandLineToolBundlePath).path
        guard FileManager.default.isExecutableFile(atPath: tool) else {
            throw Failure.toolMissing(tool)
        }

        let linkPath = HelperConstants.commandLineToolLinkPath
        let directory = (linkPath as NSString).deletingLastPathComponent
        try ensureDirectory(directory)

        if let existing = try inspect(linkPath) {
            if existing == tool { return }
            guard isReplaceable(existing) else { throw Failure.occupied(existing) }
            guard unlink(linkPath) == 0 else { throw Failure.system("unlink", errno) }
        }
        guard symlink(tool, linkPath) == 0 else { throw Failure.system("symlink", errno) }
    }

    static func uninstall() throws {
        let linkPath = HelperConstants.commandLineToolLinkPath
        guard let existing = try inspect(linkPath) else { return }
        guard isReplaceable(existing) else { throw Failure.occupied(existing) }
        guard unlink(linkPath) == 0 else { throw Failure.system("unlink", errno) }
    }

    /// nil when nothing is at `path`; the symlink target when it is a symlink.
    /// Throws `.occupied` for anything that is not a symlink.
    private static func inspect(_ path: String) throws -> String? {
        var info = stat()
        guard lstat(path, &info) == 0 else {
            if errno == ENOENT { return nil }
            throw Failure.system("lstat", errno)
        }
        guard (info.st_mode & S_IFMT) == S_IFLNK else { throw Failure.occupied(path) }
        var buffer = [CChar](repeating: 0, count: Int(PATH_MAX) + 1)
        let count = readlink(path, &buffer, buffer.count - 1)
        guard count >= 0 else { throw Failure.system("readlink", errno) }
        return String(decoding: buffer[0..<count].map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    /// Ours (points at a TetherKitNext app's bundled CLI) or dangling.
    private static func isReplaceable(_ target: String) -> Bool {
        target.hasSuffix(".app/Contents/" + HelperConstants.commandLineToolBundlePath)
            || !FileManager.default.fileExists(atPath: target)
    }

    private static func ensureDirectory(_ path: String) throws {
        var info = stat()
        if lstat(path, &info) == 0 {
            guard (info.st_mode & S_IFMT) == S_IFDIR else { throw Failure.directoryUnsafe(path) }
            return
        }
        guard errno == ENOENT else { throw Failure.system("lstat", errno) }
        // /usr/local exists on every macOS install; only bin/ may be missing
        // (Apple Silicon Macs without Homebrew in /usr/local).
        guard mkdir(path, 0o755) == 0 || errno == EEXIST else {
            throw Failure.system("mkdir", errno)
        }
        _ = chown(path, 0, 0)
    }
}
