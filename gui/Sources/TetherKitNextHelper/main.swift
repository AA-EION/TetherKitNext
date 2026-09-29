// tetherkitnext-helper -- the privileged helper running as root.
//
// * Where its root comes from *
//   From launchd: the com.tetherkitnext.helperd.plist embedded in the App (registered via SMAppService) declares
//   MachServices, and as soon as the App connects to this Mach service, launchd launches it on demand,
//   and it is root from the moment it starts. **It has nothing to do with whether the user pressed the fingerprint** -- so "who is calling" must be
//   re-verified by the helper itself every time; see TetherKitNextIPC/Authorization.swift.
//
// ★ How it is installed ★
//   Through SMAppService: the app registers Contents/Library/LaunchDaemons/
//   com.tetherkitnext.helperd.plist, whose BundleProgram points at this binary
//   inside TetherKitNext.app. launchd runs it in place (nothing is copied into
//   /Library), macOS asks the user to approve it in System Settings › Login
//   Items, and replacing the app updates the daemon. This replaced the old
//   AuthorizationExecuteWithPrivileges + setuid installer, which relied on an
//   API deprecated since macOS 10.7.
import Foundation
import TetherKitNextCore
import TetherKitNextIPC

/// Writes one line to stderr. The helper is launched by launchd, and stderr goes into the LaunchDaemon's
/// log file -- when troubleshooting problems like "the helper will not start", that is the only place where anything can be seen.
func writeToStandardError(_ message: String) {
    FileHandle.standardError.write(Data((message + "\n").utf8))
}

// ---- Only as launchd's root daemon ----
//
// Run by hand (as a user, or with sudo from a shell) the binary would register
// no Mach service, hold no session, and block forever in dispatchMain().
// Refuse clearly instead; this also makes stray invocations such as the old
// `--install` flag fail fast.
if geteuid() != 0 || getppid() != 1 {
    writeToStandardError(
        "tetherkitnext-helper is TetherKitNext's background component and is started by launchd. "
            + "Enable it from TetherKitNext.app (Settings › Background Component).")
    exit(64)  // EX_USAGE
}

// ---- Logging ----
//
// Turns on capture so the App can see the library's internal logs on the UI. Output to stderr is unaffected.
TetherKitNextLibrary.startLogCapture(level: .info)

// ---- Legacy install ----
//
// Must run before orphan cleanup: bootout makes the legacy daemon tear down
// its own feth pair, and only then are its registry entries truly orphaned.
if LegacyHelper.removeIfPresent() {
    writeToStandardError("Removed the legacy com.tetherkit.helper LaunchDaemon")
}

// ---- Backstop cleanup ----
//
// If the last run was SIGKILLed, destructors do not run and the feth NIC is still left in the kernel. This is the only place that can rescue
// that situation -- a signal handler cannot intercept SIGKILL.
do {
    let removed = try TetherKitNextLibrary.cleanupOrphanInterfaces()
    if removed > 0 {
        writeToStandardError(L(.helperOrphansCleaned, Int(removed)))
    }
} catch {
    writeToStandardError(L(.helperOrphanCleanupFailed, error.localizedDescription))
}

let service = HelperService()
let delegate = HelperListenerDelegate(service: service)

// ---- Graceful shutdown ----
//
// What `launchctl bootout` sends is SIGTERM, and Swift's deinit does not run when the process is terminated.
// Without catching it the NIC would leak in the kernel (although the on-disk registration is a backstop, cleaning up on the spot is better).
//
// DispatchSource is used rather than a signal(2) handler: the latter runs in signal context,
// where what can be done is extremely limited (no locking, no memory allocation), while the shutdown teardown we have to do
// needs both. DispatchSource turns it into a callback on an ordinary queue, and the restrictions vanish.
//
// SIG_IGN must come first: before DispatchSource takes over, the default behavior (terminating the process) is still in effect.
signal(SIGTERM, SIG_IGN)
let terminationSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
terminationSource.setEventHandler {
    // The source delivers on the main queue, which is where top-level state
    // (`service`) lives under Swift 6's main-actor isolation of main.swift.
    MainActor.assumeIsolated {
        writeToStandardError(L(.helperSigtermReceived))
        service.shutdown()
        exit(0)
    }
}
terminationSource.resume()

let listener = NSXPCListener(machServiceName: HelperConstants.machServiceName)
listener.delegate = delegate
listener.resume()

writeToStandardError(L(.helperReady, TetherKitNextLibrary.versionInfo.version))

// Block on the main runloop.
//
// Deliberately **not** doing "exit after being idle for a while": once a session is running, the helper owns the feth NIC and the
// BPF descriptor, and exiting would cut off the user's network. The LaunchDaemon is launched on demand,
// and staying alive does not occupy resources when nobody is using it -- because it was not launched at all then.
dispatchMain()
