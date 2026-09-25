import Foundation

/// App 调用 helper 的 XPC 接口。
///
/// ★ 哪些方法需要授权，哪些不需要 ★
///
///   带 `authorization` 参数的都要 —— 它是 App 通过 AuthorizationCopyRights
///   拿到的凭据的外部形式（32 字节），helper 会**复核**它确实包含所需权利。
///
///   探测类方法（helperVersion / status / queryNetwork / drainFeed）刻意**不**
///   要求授权。理由是：如果连「helper 装没装」都要先弹一次指纹，用户就分不清
///   「helper 没装」和「授权没过」这两种完全不同的失败了。这些方法只读，
///   泄漏的信息也仅限于本机网络状态。
///
/// ★ 特权方法的应答为什么是 (String?, Bool) 两个值 ★
///   第二个值表示「这次失败是授权问题」。App 需要区分这两种情况：授权过期了
///   该重新弹框重试，而操作本身失败（比如没插设备）重试多少次都一样。
///   混在一条错误消息里，App 只能去匹配字符串 —— 那是最脆的一种耦合。
///
/// ★ 为什么错误用 String? 而不是 NSError ★
///   跨 XPC 传 NSError 要求两端都能反序列化它的 userInfo，一旦里面混进不可
///   编码的对象就是运行时异常。而我们需要传给用户看的本来就只是一句中文，
///   直接传字符串既简单又不会失败。nil 表示成功。
@objc public protocol TetherKitNextHelperProtocol {
    /// 连通性探测。**不要求授权** —— 否则「没装」和「没授权」两种失败会混在一起。
    func helperVersion(reply: @escaping @Sendable (String) -> Void)

    /// 运行环境预检（root 状态、feth 的创建期 sysctl、MTU 上限）。
    func environment(reply: @escaping @Sendable (Data?, String?) -> Void)

    /// 枚举 RNDIS 设备。
    ///
    /// 由 helper 而不是 App 来枚举：App 侧也能枚举（不需要 root），但会话跑起来
    /// 之后设备已被 helper 独占，App 再去读字符串描述符只会失败。统一走 helper
    /// 就没有这个不一致。
    func listDevices(reply: @escaping @Sendable (Data?, String?) -> Void)

    /// 启动 RNDIS 会话。**需要授权。**
    func startSession(authorization: Data, configuration: Data,
                      reply: @escaping @Sendable (String?, Bool) -> Void)

    /// 停止会话并销毁虚拟网卡。**需要授权。**
    func stopSession(authorization: Data, reply: @escaping @Sendable (String?, Bool) -> Void)

    /// 取会话状态快照。
    func sessionStatus(reply: @escaping @Sendable (Data?, String?) -> Void)

    /// 给网卡下发上网方式（DHCP / 静态 IP / 撤销）。**需要授权。**
    ///
    /// DHCP 模式下这一调用会阻塞到拿到租约或超时（库内部上限 10 秒），
    /// 因此 helper 侧不能把它排在会串行阻塞其它请求的队列上。
    func applyNetwork(authorization: Data, interface: String, configuration: Data,
                      reply: @escaping @Sendable (String?, Bool) -> Void)

    /// 回读网卡真实生效的 IP 状态。
    func queryNetwork(interface: String, reply: @escaping @Sendable (Data?, String?) -> Void)

    /// 取走 helper 侧积压的日志与提示。
    func drainFeed(reply: @escaping @Sendable (Data?) -> Void)

    /// 告诉 helper 用哪种语言渲染它产生的文字。**不要求授权** —— 它只影响
    /// 文案，改不了任何行为。
    ///
    /// 为什么必须有这一条：helper 以 root 跑在 launchd 下，看不到用户的语言
    /// 偏好，而它产生的提示（「会话已停止」）与 libtetherkitnext 的日志都会原样
    /// 显示在 App 的日志卡里。不同步的话界面是一种语言、日志是另一种。
    ///
    /// 参数是 `Language` 的 rawValue（`"chinese"` / `"english"`）。传字符串而
    /// 不是整数：将来加语言时，旧 helper 收到不认识的值会原样忽略，而不是
    /// 把它当成某个碰巧存在的枚举值。
    func setLanguage(_ rawValue: String, reply: @escaping @Sendable () -> Void)

    /// Link (`install == true`) or unlink the bundled command-line tool at
    /// HelperConstants.commandLineToolLinkPath. **Requires authorization.**
    ///
    /// The link target is never taken from the caller: the daemon derives it
    /// from its own location inside the app bundle, so a client cannot use this
    /// call to plant an arbitrary root-owned symlink.
    func setCommandLineToolInstalled(authorization: Data, install: Bool,
                                     reply: @escaping @Sendable (String?, Bool) -> Void)
}
