import Foundation

/// The XPC interface through which the App calls the helper.
///
/// * Which methods need authorization and which do not *
///
///   Every one with an `authorization` parameter does -- it is the external form (32 bytes) of the credential the App obtained through
///   AuthorizationCopyRights, and the helper **verifies** that it really contains the required right.
///
///   Probe-type methods (helperVersion / status / queryNetwork / drainFeed) deliberately **do not**
///   require authorization. The reason is: if even "is the helper installed" required popping up a fingerprint first, the user could not tell apart
///   the two entirely different failures "helper not installed" and "authorization did not pass". These methods are read-only,
///   and the information they leak is limited to the local machine's network state.
///
/// * Why the reply of privileged methods is two values (String?, Bool) *
///   The second value means "this failure is an authorization problem". The App needs to tell the two cases apart: an expired authorization
///   should pop up the dialog again and retry, while if the operation itself failed (say no device plugged in), retrying any number of times is the same.
///   Mixed into one error message, the App could only match strings -- the most brittle kind of coupling.
///
/// * Why errors use String? rather than NSError *
///   Passing NSError across XPC requires both ends to be able to deserialize its userInfo, and once an object that cannot be
///   encoded slips in, it is a runtime exception. And what we need to show the user is only one sentence of text anyway,
///   so passing a string is both simple and cannot fail. nil means success.
@objc public protocol TetherKitNextHelperProtocol {
    /// Connectivity probe. **Does not require authorization** -- otherwise the two failures "not installed" and "not authorized" would be mixed together.
    func helperVersion(reply: @escaping @Sendable (String) -> Void)

    /// Preflight of the runtime environment (root status, feth's creation-time sysctls, MTU upper limit).
    func environment(reply: @escaping @Sendable (Data?, String?) -> Void)

    /// Enumerates RNDIS devices.
    ///
    /// Enumerated by the helper rather than the App: the App side can also enumerate (no root needed), but once a session is running
    /// the device is held exclusively by the helper, and the App reading string descriptors would only fail. Going through the helper uniformly
    /// avoids this inconsistency.
    func listDevices(reply: @escaping @Sendable (Data?, String?) -> Void)

    /// Starts an RNDIS session. **Needs authorization.**
    func startSession(authorization: Data, configuration: Data,
                      reply: @escaping @Sendable (String?, Bool) -> Void)

    /// Stops the session and destroys the virtual NIC. **Needs authorization.**
    func stopSession(authorization: Data, reply: @escaping @Sendable (String?, Bool) -> Void)

    /// Takes a snapshot of the session state.
    func sessionStatus(reply: @escaping @Sendable (Data?, String?) -> Void)

    /// Applies a connectivity method (DHCP / static IP / revoke) to the NIC. **Needs authorization.**
    ///
    /// In DHCP mode this call blocks until a lease is obtained or it times out (the library's internal cap is 10 seconds),
    /// so the helper side must not queue it on a queue that would serially block other requests.
    func applyNetwork(authorization: Data, interface: String, configuration: Data,
                      reply: @escaping @Sendable (String?, Bool) -> Void)

    /// Reads back the IP state the NIC actually has in effect.
    func queryNetwork(interface: String, reply: @escaping @Sendable (Data?, String?) -> Void)

    /// Takes away the logs and hints accumulated on the helper side.
    func drainFeed(reply: @escaping @Sendable (Data?) -> Void)

    /// Tells the helper which language to render the text it produces in. **Does not require authorization** -- it only affects
    /// messages and cannot change any behavior.
    ///
    /// Why this one must exist: the helper runs as root under launchd and cannot see the user's language
    /// preference, yet the hints it produces ("session stopped") and libtetherkitnext's logs are both shown as-is
    /// in the App's log card. Without synchronization the UI would be in one language and the logs in another.
    ///
    /// The parameter is the rawValue of `Language` (`"chinese"` / `"english"`). A string is passed rather than
    /// an integer: when a language is added in the future, an old helper receiving an unrecognized value simply ignores it, rather than
    /// treating it as some enum value that happens to exist.
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
