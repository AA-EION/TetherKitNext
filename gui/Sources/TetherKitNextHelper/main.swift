// tetherkitnext-helper —— 以 root 运行的特权 helper。
//
// ★ 它的 root 从哪来 ★
//   来自 launchd：App 内嵌的 com.tetherkitnext.helperd.plist（SMAppService 注册）声明了
//   MachServices，App 一连上这个 Mach 服务，launchd 就按需把它拉起来，
//   一启动就是 root。**跟用户按没按指纹毫无关系** —— 所以「谁在调用」必须由
//   helper 自己每次复核，见 TetherKitNextIPC/Authorization.swift。
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

/// 往 stderr 写一行。helper 由 launchd 拉起，stderr 进的是 LaunchDaemon 的
/// 日志文件 —— 排查「helper 起不来」这类问题时，那是唯一能看到东西的地方。
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

// ---- 日志 ----
//
// 打开捕获，让 App 能在界面上看到库内部的日志。stderr 的输出不受影响。
TetherKitNextLibrary.startLogCapture(level: .info)

// ---- Legacy install ----
//
// Must run before orphan cleanup: bootout makes the legacy daemon tear down
// its own feth pair, and only then are its registry entries truly orphaned.
if LegacyHelper.removeIfPresent() {
    writeToStandardError("Removed the legacy com.tetherkit.helper LaunchDaemon")
}

// ---- 兜底清理 ----
//
// 上一次运行若被 SIGKILL，析构不会跑，feth 网卡还留在内核里。这是唯一能救回
// 那种情况的地方 —— 信号处理器拦不住 SIGKILL。
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

// ---- 优雅停机 ----
//
// `launchctl bootout` 发的是 SIGTERM，而 Swift 的 deinit 在进程被终止时不会跑。
// 不接住它就会把网卡漏在内核里（虽然有落盘登记兜底，但能当场清干净更好）。
//
// 用 DispatchSource 而不是 signal(2) 的处理函数：后者跑在信号上下文里，
// 那里能做的事极其有限（不能加锁、不能分配内存），而我们要做的停机拆除
// 恰恰两样都要。DispatchSource 把它转成普通队列上的回调，限制就没了。
//
// 必须先 SIG_IGN：在 DispatchSource 接手之前，默认行为（终止进程）仍然生效。
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

// 阻塞在主 runloop 上。
//
// 刻意**不**做「空闲一段时间就退出」：会话跑起来之后，helper 拥有 feth 网卡与
// BPF 描述符，退出就等于把用户的网络断掉。LaunchDaemon 是按需拉起的，
// 一直活着并不会在没人用时占资源 —— 因为那时根本没被拉起来。
dispatchMain()
