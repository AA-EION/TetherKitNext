// TetherKitNext's C ABI -- the **only** `extern "C"` boundary in the whole project.
//
// Reason for existing: Swift's C++ interop cannot swallow this project's C++23 interfaces (`std::expected`,
// `std::span`, abstract base classes, move semantics), so the GUI cannot call the C++ layer directly. This layer translates those
// types into "plain PODs + opaque handles", so Swift, Objective-C, and even other languages can use it.
//
// * Design conventions (follow them and nothing goes wrong) *
//
//   1. **No ownership crosses the boundary.** All outputs are written into fixed-size structs provided by the caller, and the library does not
//      allocate memory internally that the caller has to free. The only exception is the opaque handle `tk_session_t`,
//      which is created by `tk_session_create` and destroyed by `tk_session_destroy`, a clearly paired lifecycle.
//
//   2. **No callbacks; use an event queue.** The underlying observer callbacks come from the libusb event thread and the control
//      thread, so the Swift side would have to marshal across threads; worse still is reentrancy -- calling stop from inside a callback
//      is a self-wait deadlock (`Stop()` has to join the control thread). So everything is turned into "queue inside the library, host polls".
//      Logs are the same; see `tk_drain_logs`.
//
//   3. **All functions return `tk_result_t` (0 for success, negative for failure)**, and the failure cause is written into
//      an optional `tk_error_t*`. Passing NULL means you do not care about the cause. No errno and no thread-local
//      last-error is used -- both are easy to misread in a multi-threaded host.
//
//   4. **Strings are always fixed-size UTF-8 buffers, NUL-terminated**, truncated rather than failing when capacity is insufficient.
//      Truncation affects only display, not functionality, and is much simpler than making the caller handle a two-call length negotiation.
//
// * Privileges (which functions need root) *
//
//   No root needed: `tk_version`, `tk_check_environment`, `tk_list_devices`,
//                `tk_drain_logs`, `tk_net_query`
//   Root needed   : `tk_session_*` (creates feth, opens /dev/bpf*), `tk_net_apply`,
//                `tk_net_clear`, `tk_cleanup_orphan_interfaces`
//
//   The GUI's approach: the App itself only calls the ones that need no root, and hands the ones that need root to
//   `tetherkitnext-helper` (launched by launchd as root). See docs/GUI-ARCHITECTURE.md for details.
#ifndef TETHERKITNEXT_CAPI_TETHERKITNEXT_C_H_
#define TETHERKITNEXT_CAPI_TETHERKITNEXT_C_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
#define TK_API
#else
#define TK_API __attribute__((visibility("default")))
#endif

// =============================================================================
// Capacity constants
//
// The reason capacities are hard-coded in the header rather than having the caller negotiate lengths: these structs are mapped directly by Swift
// into structs and copied by value, and only fixed sizes eliminate all memory management. The values all leave ample margin.
// =============================================================================

/// Capacity of the error message buffer. Underlying error strings carry multi-line "what to do next" hints; 1 KiB is enough.
#define TK_ERROR_MESSAGE_CAPACITY 1024
/// Capacity of a NIC name. Aligned with the kernel's IFNAMSIZ (16, including the terminator).
#define TK_INTERFACE_NAME_CAPACITY 16
/// Capacity of USB string descriptors (vendor name / product name / serial number).
#define TK_USB_STRING_CAPACITY 128
/// Capacity of IP address text. Aligned with INET6_ADDRSTRLEN (46).
#define TK_ADDRESS_CAPACITY 46
/// The maximum number of DNS servers accepted in one configuration.
#define TK_DNS_MAX 4
/// Text capacity of a single log / event.
#define TK_TEXT_CAPACITY 512
/// Capacity of a thread name, aligned with the actual limit of pthread_setname_np.
#define TK_THREAD_NAME_CAPACITY 16

// =============================================================================
// Result codes and errors
// =============================================================================

typedef enum tk_result {
  TK_OK = 0,                    ///< Success.
  TK_ERR_INVALID_ARGUMENT = -1, ///< Invalid argument (null pointer, out-of-range enum, malformed address).
  TK_ERR_INVALID_STATE = -2,    ///< The operation is not allowed in the current state (such as a repeated start).
  TK_ERR_FAILED = -3,           ///< The underlying operation failed; see tk_error_t for details.
  TK_ERR_PERMISSION = -4,       ///< Root is needed but the process is not root.
  TK_ERR_NOT_FOUND = -5,        ///< The target does not exist (no device, no such NIC).
} tk_result_t;

/// The source domain of an error code, which determines how the `code` field is interpreted. It and the C++ side's `tetherkitnext::ErrorDomain`
/// correspond one-to-one.
typedef enum tk_error_domain {
  TK_ERROR_DOMAIN_GENERIC = 0, ///< Pure logic error; `code` is meaningless.
  TK_ERROR_DOMAIN_ERRNO = 1,   ///< POSIX errno.
  TK_ERROR_DOMAIN_LIBUSB = 2,  ///< libusb's enum libusb_error (negative values).
  TK_ERROR_DOMAIN_RNDIS = 3,   ///< RNDIS_STATUS_*.
} tk_error_domain_t;

/// A complete description of a failure. `message` is already text that can be shown directly to the user, in the current interface language.
typedef struct tk_error {
  int32_t domain; ///< tk_error_domain_t
  int64_t code;
  char message[TK_ERROR_MESSAGE_CAPACITY];
} tk_error_t;

// =============================================================================
// Interface language
//
// All text the library itself produces -- error messages, log lines, event text -- is rendered in the language set here.
// The host (the GUI) is responsible for pushing the user's choice in; the command line decides by environment variables and --lang itself.
//
// **A single process-wide state**, not one per session: logs and errors are produced from multiple threads, and making it
// thread-local would only make the output of a single session appear in two languages.
// =============================================================================

typedef enum tk_language {
  TK_LANGUAGE_ENGLISH = 0,
  TK_LANGUAGE_CHINESE = 1,
} tk_language_t;

/// Sets the interface language. Callable from any thread, at any time.
///
/// Takes effect immediately, but **logs and events already queued are not re-rendered** -- they were already
/// strings at the moment they were produced. The GUI should call it as early as possible at startup, and afterwards only when the user switches language.
/// Ignores the call when `language` is out of range.
TK_API void tk_set_language(int32_t language);

/// Reads the current interface language (tk_language_t).
TK_API int32_t tk_get_language(void);

/// Infers the language from environment variables, checking in order TETHERKITNEXT_LANG, LC_ALL, LC_MESSAGES, LANG.
///
/// **Only infers; does not change the current setting.** The GUI usually has no use for it (macOS language preferences should be asked of
/// NSLocale); it is prepared for command-line hosts and third-party bindings.
TK_API int32_t tk_detect_system_language(void);

// =============================================================================
// Version and environment preflight (none need root)
// =============================================================================

typedef struct tk_version_info {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
  /// In the form "TetherKitNext 0.1.1 (C++23, macOS 13.3+)".
  char text[64];
  /// Build configuration description, in the form "build 1a2b3c4d5e, RelWithDebInfo, AppleClang 21.0.0, ...".
  /// The leading build ID (git commit) distinguishes builds that share a
  /// version number; the app compares it with the background daemon's.
  char build[192];
  /// libusb version string.
  char libusb[64];
} tk_version_info_t;

TK_API void tk_version(tk_version_info_t* out_version);

/// Environment preflight result. The GUI shows it before startup, explaining "why it cannot run" up front.
typedef struct tk_environment {
  /// Whether the current process runs as root.
  bool is_root;
  /// Whether feth's creation-time sysctls are all at the values we require.
  ///
  /// These switches (hwcsum / fcs / tso_support / lro / trailer_length /
  /// separate_frame_header) are **creation-time snapshots**; changing them after creation has no effect, so they must be checked in advance.
  bool sysctls_ok;
  /// Specific explanation when the sysctls are unacceptable; an empty string when acceptable.
  char sysctl_detail[TK_ERROR_MESSAGE_CAPACITY];
  /// The upper limit of MTU supported by feth (sysctl net.link.fake.max_mtu). 0 when the query fails.
  uint32_t feth_max_mtu;
} tk_environment_t;

/// Collects environment information. **This function always returns TK_OK** -- "the environment is unacceptable" is not a call failure,
/// but a result to be shown to the user.
TK_API tk_result_t tk_check_environment(tk_environment_t* out_environment);

// =============================================================================
// Device enumeration (no root needed)
// =============================================================================

/// A candidate device recognized as RNDIS.
typedef struct tk_device_info {
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t bus_number;
  uint8_t device_address;
  /// Communications-class interface (carries the control channel + interrupt notifications).
  uint8_t control_interface;
  /// Data-class interface (carries bulk IN/OUT).
  uint8_t data_interface;
  /// The matched interface signature, used to explain "which RNDIS form it was recognized as".
  uint8_t interface_class;
  uint8_t interface_subclass;
  uint8_t interface_protocol;
  /// Whether the Android quirk fallback path was taken (when the CDC Union descriptor is untrustworthy).
  bool used_android_quirk;
  /// The following three string descriptors are **best effort**: reading them needs libusb_open,
  /// and the device may already be held exclusively by another process on this machine. When they cannot be read this time, the value most recently
  /// read successfully **within this process** is filled back (the device has not changed, so its name should not vanish just because it is occupied); only
  /// if there is not even a previous value is it an empty string, in which case the GUI should fall back to showing `description` (the VID:PID form).
  char manufacturer[TK_USB_STRING_CAPACITY];
  char product[TK_USB_STRING_CAPACITY];
  char serial[TK_USB_STRING_CAPACITY];
  /// In the form "Bus 020 Device 003: 18d1:4ee4", usable in any situation.
  char description[64];
} tk_device_info_t;

/// Enumerates the currently connected RNDIS devices.
///
/// @param out_devices    Array provided by the caller; may be NULL (in which case only the count is computed).
/// @param capacity       Array capacity. When more devices are actually found than the capacity, only the first capacity are filled.
/// @param out_count      Receives the total number of devices actually found (may be greater than capacity).
/// @param read_strings   Whether to try reading the vendor name / product name / serial number. This needs
///                       libusb_open, and fails when the device is already occupied (not fatal).
///                       Passing false is recommended when refreshing the list while a session is running -- whether skipped or
///                       failed to read, the value most recently read successfully within this process is filled back, so the name does not
///                       become an empty string because of that.
TK_API tk_result_t tk_list_devices(tk_device_info_t* out_devices, size_t capacity,
                                   size_t* out_count, bool read_strings, tk_error_t* out_error);

// =============================================================================
// Logs
// =============================================================================

typedef enum tk_log_level {
  TK_LOG_TRACE = 0,
  TK_LOG_DEBUG = 1,
  TK_LOG_INFO = 2,
  TK_LOG_WARN = 3,
  TK_LOG_ERROR = 4,
  TK_LOG_OFF = 5,
} tk_log_level_t;

typedef struct tk_log_record {
  int32_t level; ///< tk_log_level_t
  /// Wall-clock time (nanoseconds since the Unix epoch). Wall time is used rather than monotonic time: logs must be able to line up with the system
  /// log.
  int64_t wall_nanos;
  char thread[TK_THREAD_NAME_CAPACITY];
  char message[TK_TEXT_CAPACITY];
} tk_log_record_t;

TK_API void tk_set_log_level(int32_t level);

/// Opens / closes log capture.
///
/// **Off** by default -- command-line programs do not need it, and leaving it on wastes an extra copy. The GUI host calls
/// `tk_enable_log_capture(true)` at startup, and afterwards periodically `tk_drain_logs` to take them away.
TK_API void tk_enable_log_capture(bool enabled);

/// Takes away buffered log lines (first in, first out), returning the number actually taken.
///
/// The buffer is a fixed-size ring queue; when full, it discards the **oldest** records -- keeping the latest scene when the host is stuck
/// is more useful than keeping the beginning. The number of dropped records is reported through `out_dropped`, and the GUI can use it to show
/// "the log has gaps".
TK_API size_t tk_drain_logs(tk_log_record_t* out_records, size_t capacity,
                            uint64_t* out_dropped);

// =============================================================================
// Sessions (root needed)
// =============================================================================

/// RNDIS host-side state, corresponding one-to-one with the C++ side's `tetherkitnext::rndis::State`.
typedef enum tk_rndis_state {
  TK_RNDIS_UNINITIALIZED = 0,
  TK_RNDIS_INITIALIZING = 1,
  TK_RNDIS_INITIALIZED = 2,
  TK_RNDIS_DATA_INITIALIZED = 3, ///< Data starts flowing.
  TK_RNDIS_HALTING = 4,
} tk_rndis_state_t;

/// The lifecycle state of a session. This is the state the GUI's main screen binds to directly.
typedef enum tk_run_state {
  TK_RUN_IDLE = 0,     ///< Created, not yet started.
  TK_RUN_STARTING = 1, ///< Running the startup sequence (enumerate -> handshake -> create NIC -> open bridge).
  TK_RUN_RUNNING = 2,  ///< The data path is up and running.
  TK_RUN_STOPPING = 3,
  TK_RUN_STOPPED = 4,
  TK_RUN_FAILED = 5, ///< Startup failed or an unrecoverable error occurred while running; see status.fatal for the cause.
} tk_run_state_t;

/// Session configuration. Fill in the defaults with `tk_session_config_init` and then change the fields that need changing --
/// initializing directly with `{0}` gives a bunch of invalid zeros.
typedef struct tk_session_config {
  /// Device filter. All 0 means use the first RNDIS device found.
  uint16_t vendor_id;
  uint16_t product_id;
  /// Match only the specified address on the specified bus (both must be given together). Used to tell apart two devices of the same model.
  uint8_t bus_number;
  uint8_t device_address;

  /// The desired MTU. Lowered by negotiation when the device cannot fit it; the upper limit is constrained by feth's
  /// sysctl net.link.fake.max_mtu.
  uint32_t mtu;
  /// Whether to set the system-side NIC's MAC to the address the device reported. On by default.
  bool adopt_device_mac;

  /// Number of concurrent in-flight bulk IN / OUT transfers.
  uint32_t rx_transfer_count;
  uint32_t tx_transfer_count;
  /// KiB per bulk IN buffer.
  uint32_t rx_transfer_kib;
  /// The MaxTransferSize claimed in INITIALIZE_MSG (KiB). This is the only means of getting the device to aggregate multiple packets,
  /// and also the main lever for throughput.
  uint32_t max_transfer_kib;
  /// KiB of the BPF kernel capture buffer.
  uint32_t bpf_buffer_kib;
} tk_session_config_t;

/// Fills the configuration with recommended defaults.
TK_API void tk_session_config_init(tk_session_config_t* out_config);

/// Session events.
typedef enum tk_event_kind {
  /// RNDIS state transition. `a` = state before the transition, `b` = state after (both tk_rndis_state_t).
  TK_EVENT_RNDIS_STATE = 0,
  /// Negotiation complete. `a` = final MTU, `b` = link speed (Mbps).
  TK_EVENT_NEGOTIATED = 1,
  /// Link up / down. `a` = 1 means connected.
  TK_EVENT_LINK = 2,
  /// Device soft reset. `a` = 1 means addressing information was lost and has been replayed.
  TK_EVENT_DEVICE_RESET = 3,
  /// Unrecoverable error; `text` is the cause. The session then enters TK_RUN_FAILED.
  TK_EVENT_FATAL = 4,
  /// Session lifecycle state change. `a` = before, `b` = after (both tk_run_state_t).
  TK_EVENT_RUN_STATE = 5,
} tk_event_kind_t;

typedef struct tk_event {
  int32_t kind; ///< tk_event_kind_t
  int64_t wall_nanos;
  int64_t a;
  int64_t b;
  char text[TK_TEXT_CAPACITY];
} tk_event_t;

/// A complete snapshot of the data path and link. The GUI pulls it periodically (500 ms~1 s recommended) to refresh the UI.
typedef struct tk_session_status {
  int32_t run_state;   ///< tk_run_state_t
  int32_t rndis_state; ///< tk_rndis_state_t
  bool link_up;
  bool paused; ///< Whether data movement is paused (during link down or device reset).

  /// System-side NIC name (the host configures IP on this one). An empty string when not created.
  char system_interface[TK_INTERFACE_NAME_CAPACITY];
  /// Driver-side NIC name (TetherKitNext's BPF attaches to this one). For troubleshooting display only.
  char driver_interface[TK_INTERFACE_NAME_CAPACITY];

  /// The permanent MAC reported by the device. All 0 when not negotiated.
  uint8_t device_mac[6];
  /// The final MTU after negotiation.
  uint32_t mtu;
  /// Link speed (Mbps).
  uint32_t link_speed_mbps;
  /// The device's vendor description string (from OID_GEN_VENDOR_DESCRIPTION).
  char vendor_description[64];
  /// The device in use, in the form "Bus 020 Device 003: 18d1:4ee4".
  char device_description[64];

  /// Cumulative counters (RX = device -> host, TX = host -> device).
  /// Rates are computed by the GUI itself as the difference of two snapshots; the library does no window smoothing.
  uint64_t rx_frames;
  uint64_t rx_bytes;
  uint64_t rx_dropped;
  uint64_t tx_frames;
  uint64_t tx_bytes;
  uint64_t tx_dropped;
  /// Current depth of the RX queue, used to judge which side is the bottleneck.
  uint64_t rx_queue_depth;
  /// Kernel BPF-side cumulative drops (bs_drop).
  uint64_t link_kernel_drops;
  /// Number of backpressure events caused by the TX transfer pool having no free slot.
  uint64_t tx_backpressure;

  /// Monotonic nanosecond count at the sampling moment. The GUI uses it to compute rates; do not use your own clock --
  /// the real interval between two pulls may be stretched by scheduling.
  int64_t monotonic_nanos;

  /// The cause when run_state == TK_RUN_FAILED; otherwise an empty string.
  char fatal[TK_ERROR_MESSAGE_CAPACITY];
} tk_session_status_t;

/// Opaque session handle.
typedef struct tk_session tk_session_t;

/// Creates a session. **Does no I/O**, just records the configuration.
TK_API tk_session_t* tk_session_create(const tk_session_config_t* config, tk_error_t* out_error);

/// Starts a session. **Non-blocking**: internally starts a control thread to run the startup sequence and the keepalive loop, and this function returns immediately.
///
/// Returning TK_OK only means "the start request has been accepted"; real success or failure must be observed through the run_state of `tk_session_status`
/// and the event queue. It is designed this way because the startup sequence includes a USB handshake, which on slow devices
/// may take hundreds of milliseconds to several seconds, and blocking the GUI main thread is unacceptable.
///
/// The control channel must run on a **non-libusb-event thread** (the synchronous API returns on the event thread
/// LIBUSB_ERROR_BUSY); the library guarantees this internally, and callers need not care about threads.
TK_API tk_result_t tk_session_start(tk_session_t* session, tk_error_t* out_error);

/// Requests shutdown and waits for the control thread to exit. Idempotent.
///
/// WARNING: **Must not be called from a callback of event polling** -- this function joins the control thread. The C ABI layer
/// has no callbacks, so it is fine as long as the caller does not hold, inside its own event handling, a lock that could cause reentrancy.
TK_API tk_result_t tk_session_stop(tk_session_t* session);

/// Stops and destroys. Passing NULL is a no-op.
TK_API void tk_session_destroy(tk_session_t* session);

/// Takes a state snapshot. Callable from any thread.
TK_API tk_result_t tk_session_status_get(tk_session_t* session, tk_session_status_t* out_status);

/// Takes away queued events (first in, first out), returning the number actually taken.
///
/// Like logs, it is a fixed-size ring queue that drops the oldest when full. The GUI missing a few state-transition events does not affect correctness
/// -- the authoritative state is always the `tk_session_status_get` snapshot, and events are only used for animation and hints.
TK_API size_t tk_session_poll_events(tk_session_t* session, tk_event_t* out_events,
                                     size_t capacity);

// =============================================================================
// NIC IP configuration (root needed)
// =============================================================================

/// Connection method.
typedef enum tk_ip_mode {
  /// Hand it to the system's IPConfiguration to run DHCP. It automatically does four things: get a lease, configure
  /// scoped DNS, install a scoped default route, and publish the service to the dynamic store.
  TK_IP_MODE_DHCP = 0,
  /// Static IP. The address is still applied through IPConfiguration (rather than a bare SIOCAIFADDR),
  /// so that a service "registered through the proper path" is obtained -- hand-writing entries into the dynamic store is
  /// **not adopted by IPMonitor**, which has been confirmed by measurement.
  TK_IP_MODE_MANUAL = 1,
  /// Revokes the configuration (equivalent to `ipconfig set <if> NONE`).
  TK_IP_MODE_NONE = 2,
} tk_ip_mode_t;

typedef struct tk_ip_config {
  int32_t mode; ///< tk_ip_mode_t

  /// The following four items are used only when mode == TK_IP_MODE_MANUAL.
  char address[TK_ADDRESS_CAPACITY];
  char netmask[TK_ADDRESS_CAPACITY];
  /// Router (gateway). Empty means no default route is configured.
  char router[TK_ADDRESS_CAPACITY];
  char dns[TK_DNS_MAX][TK_ADDRESS_CAPACITY];
  int32_t dns_count;

  /// Whether to point the **global** default route at this NIC as well.
  ///
  /// When off there is only one scoped default route (`RTF_IFSCOPE`), which traffic bound to this interface uses,
  /// while unbound traffic still goes through the system's primary service. It only needs to be turned on when a higher-priority connected service (Wi-Fi,
  /// VPN) exists at the same time -- the typical USB tethering scenario is precisely that no other network is available,
  /// in which case this NIC is naturally the primary service, and this switch need not be touched.
  bool set_default_route;
} tk_ip_config_t;

/// Fills the configuration with defaults (DHCP).
TK_API void tk_ip_config_init(tk_ip_config_t* out_config);

/// The IP state that the NIC **actually has in effect** at present.
///
/// Deliberately does not restate "what we applied" but reads back the system state: in static mode, whether DNS takes effect
/// depends on whether IPMonitor accepts this service, and only a readback gives the user accurate feedback.
typedef struct tk_net_state {
  bool has_address;
  char address[TK_ADDRESS_CAPACITY];
  char netmask[TK_ADDRESS_CAPACITY];
  char router[TK_ADDRESS_CAPACITY];
  char dns[TK_DNS_MAX][TK_ADDRESS_CAPACITY];
  int32_t dns_count;
  /// The configuration method reported by IPConfiguration ("DHCP" / "MANUAL" / "NONE" / "").
  char method[16];
  /// The service state reported by IPConfiguration (such as "BOUND").
  char service_state[24];
  /// Whether a default route exists on this interface (scoped or global).
  bool has_default_route;
  /// Whether the global default route currently points at this interface.
  bool is_primary_default_route;
} tk_net_state_t;

/// Configures the NIC's IP according to the configuration. Needs root.
TK_API tk_result_t tk_net_apply(const char* interface_name, const tk_ip_config_t* config,
                                tk_error_t* out_error);

/// Revokes the IP configuration on the NIC (`ipconfig set <if> NONE`). Needs root.
TK_API tk_result_t tk_net_clear(const char* interface_name, tk_error_t* out_error);

/// Reads back the IP state the NIC actually has in effect. **No root needed**.
TK_API tk_result_t tk_net_query(const char* interface_name, tk_net_state_t* out_state,
                                tk_error_t* out_error);

// =============================================================================
// Orphan NIC cleanup (root needed)
// =============================================================================

/// Destroys feth interfaces left in the kernel after a process was forcibly killed.
///
/// Why it is needed: when a process is SIGKILLed, C++ destructors do not run and the feth stays in the kernel; signal
/// handlers cannot intercept SIGKILL, so the only option is a **backstop cleanup on the next launch**. When a session creates a feth,
/// it records the interface name into `/var/run/tetherkitnext-interfaces`, and this function reads it and destroys them one by one.
///
/// @param out_removed Receives the number of interfaces actually destroyed; may be NULL.
TK_API tk_result_t tk_cleanup_orphan_interfaces(size_t* out_removed, tk_error_t* out_error);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // TETHERKITNEXT_CAPI_TETHERKITNEXT_C_H_
