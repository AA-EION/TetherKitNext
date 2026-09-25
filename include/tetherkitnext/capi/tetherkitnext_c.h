// TetherKitNext 的 C ABI —— 全项目**唯一**的 `extern "C"` 边界。
//
// 存在理由：Swift 的 C++ 互操作吞不下本项目的 C++23 接口（`std::expected`、
// `std::span`、抽象基类、移动语义），所以 GUI 不能直接调 C++ 层。这一层把那些
// 类型翻译成「纯 POD + 不透明句柄」，让 Swift、Objective-C、乃至其它语言都能用。
//
// ★ 设计约定（照着写就不会出错）★
//
//   1. **不跨边界传所有权。** 所有输出都写进调用方提供的定长结构体，库内部不
//      分配需要调用方释放的内存。唯一的例外是 `tk_session_t` 这个不透明句柄，
//      它由 `tk_session_create` 创建、`tk_session_destroy` 销毁，配对明确。
//
//   2. **不用回调，用事件队列。** 底层的观察者回调会从 libusb 事件线程与控制
//      线程上来，Swift 侧要跨线程 marshal；更要命的是重入 —— 在回调里调停机会
//      自等死锁（`Stop()` 要 join 控制线程）。所以一律改成「库内排队、宿主轮询」。
//      日志同理，见 `tk_drain_logs`。
//
//   3. **所有函数返回 `tk_result_t`（0 为成功、负值为失败）**，失败原因写进
//      可选的 `tk_error_t*`。传 NULL 表示不关心原因。不用 errno、不用线程局部
//      的 last-error —— 那两种在多线程宿主里都容易读错。
//
//   4. **字符串一律是定长 UTF-8 缓冲、以 NUL 结尾**，容量不足时截断而非失败。
//      截断只影响展示，不影响功能，比让调用方处理两次调用的长度协商简单得多。
//
// ★ 权限（哪些函数需要 root）★
//
//   不需要 root：`tk_version`、`tk_check_environment`、`tk_list_devices`、
//                `tk_drain_logs`、`tk_net_query`
//   需要 root  ：`tk_session_*`（要建 feth、开 /dev/bpf*）、`tk_net_apply`、
//                `tk_net_clear`、`tk_cleanup_orphan_interfaces`
//
//   GUI 的做法是：App 本体只调不需要 root 的那些，需要 root 的交给
//   `tetherkitnext-helper`（由 launchd 以 root 拉起）。详见 docs/GUI-ARCHITECTURE.md。
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
// 容量常量
//
// 之所以把容量写死在头文件里而不是让调用方协商长度：这些结构体会被 Swift 直接
// 映射成 struct 并按值拷贝，定长才能省掉全部的内存管理。数值都留了足够余量。
// =============================================================================

/// 错误消息缓冲容量。底层错误串会带多行「接下来该怎么办」的提示，1 KiB 够用。
#define TK_ERROR_MESSAGE_CAPACITY 1024
/// 网卡名容量。对齐内核的 IFNAMSIZ（16，含终止符）。
#define TK_INTERFACE_NAME_CAPACITY 16
/// USB 字符串描述符（厂商名 / 产品名 / 序列号）容量。
#define TK_USB_STRING_CAPACITY 128
/// IP 地址文本容量。对齐 INET6_ADDRSTRLEN（46）。
#define TK_ADDRESS_CAPACITY 46
/// 一次配置最多接受几个 DNS 服务器。
#define TK_DNS_MAX 4
/// 单条日志 / 事件的文本容量。
#define TK_TEXT_CAPACITY 512
/// 线程名容量，对齐 pthread_setname_np 的实际上限。
#define TK_THREAD_NAME_CAPACITY 16

// =============================================================================
// 结果码与错误
// =============================================================================

typedef enum tk_result {
  TK_OK = 0,                    ///< 成功。
  TK_ERR_INVALID_ARGUMENT = -1, ///< 参数非法（空指针、越界枚举、格式错误的地址）。
  TK_ERR_INVALID_STATE = -2,    ///< 当前状态下不允许该操作（如重复 start）。
  TK_ERR_FAILED = -3,           ///< 底层操作失败，细节见 tk_error_t。
  TK_ERR_PERMISSION = -4,       ///< 需要 root 但当前不是。
  TK_ERR_NOT_FOUND = -5,        ///< 目标不存在（没有设备、没有该网卡）。
} tk_result_t;

/// 错误码的来源域，决定 `code` 字段怎么解读。与 C++ 侧 `tetherkitnext::ErrorDomain`
/// 一一对应。
typedef enum tk_error_domain {
  TK_ERROR_DOMAIN_GENERIC = 0, ///< 纯逻辑错误，`code` 无意义。
  TK_ERROR_DOMAIN_ERRNO = 1,   ///< POSIX errno。
  TK_ERROR_DOMAIN_LIBUSB = 2,  ///< libusb 的 enum libusb_error（负值）。
  TK_ERROR_DOMAIN_RNDIS = 3,   ///< RNDIS_STATUS_*。
} tk_error_domain_t;

/// 一次失败的完整描述。`message` 已经是可以直接展示给用户的中文。
typedef struct tk_error {
  int32_t domain; ///< tk_error_domain_t
  int64_t code;
  char message[TK_ERROR_MESSAGE_CAPACITY];
} tk_error_t;

// =============================================================================
// 界面语言
//
// 库自己产生的全部文字 —— 错误消息、日志行、事件文本 —— 都按这里设定的语言
// 渲染。宿主（GUI）负责把用户的选择推进来；命令行自己按环境变量与 --lang 决定。
//
// **进程级的单一状态**，不是每个会话一份：日志与错误从多个线程产生，做成
// 线程局部只会让同一次会话的输出出现两种语言。
// =============================================================================

typedef enum tk_language {
  TK_LANGUAGE_ENGLISH = 0,
  TK_LANGUAGE_CHINESE = 1,
} tk_language_t;

/// 设置界面语言。任意线程可调，随时可调。
///
/// 立即生效，但**已经排队的日志与事件不会重新渲染** —— 它们在产生的那一刻就
/// 已经是字符串了。GUI 应在启动时尽早调用，之后只在用户切换语言时再调。
/// `language` 越界时忽略本次调用。
TK_API void tk_set_language(int32_t language);

/// 读当前界面语言（tk_language_t）。
TK_API int32_t tk_get_language(void);

/// 按环境变量推断语言，依次看 TETHERKITNEXT_LANG、LC_ALL、LC_MESSAGES、LANG。
///
/// **只做推断，不改变当前设置。** GUI 通常用不上它（macOS 的语言偏好该问
/// NSLocale），它是给命令行宿主和第三方绑定准备的。
TK_API int32_t tk_detect_system_language(void);

// =============================================================================
// 版本与环境预检（均不需要 root）
// =============================================================================

typedef struct tk_version_info {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
  /// 形如 "TetherKitNext 0.1.1 (C++23, macOS 13.3+)"。
  char text[64];
  /// 构建配置描述，形如 "build 1a2b3c4d5e, RelWithDebInfo, AppleClang 21.0.0, ..."。
  /// The leading build ID (git commit) distinguishes builds that share a
  /// version number; the app compares it with the background daemon's.
  char build[192];
  /// libusb 版本串。
  char libusb[64];
} tk_version_info_t;

TK_API void tk_version(tk_version_info_t* out_version);

/// 环境预检结果。GUI 在启动前展示它，把「为什么跑不起来」提前说清楚。
typedef struct tk_environment {
  /// 当前进程是否以 root 运行。
  bool is_root;
  /// feth 的创建期 sysctl 是否都处于我们要求的值。
  ///
  /// 这批开关（hwcsum / fcs / tso_support / lro / trailer_length /
  /// separate_frame_header）是**创建期快照**，创建后再改无效，所以必须提前检查。
  bool sysctls_ok;
  /// sysctl 不合格时的具体说明；合格时为空串。
  char sysctl_detail[TK_ERROR_MESSAGE_CAPACITY];
  /// feth 支持的 MTU 上限（sysctl net.link.fake.max_mtu）。查询失败时为 0。
  uint32_t feth_max_mtu;
} tk_environment_t;

/// 采集环境信息。**本函数永远返回 TK_OK** —— 「环境不合格」不是调用失败，
/// 而是要展示给用户的结果。
TK_API tk_result_t tk_check_environment(tk_environment_t* out_environment);

// =============================================================================
// 设备枚举（不需要 root）
// =============================================================================

/// 一个被识别为 RNDIS 的候选设备。
typedef struct tk_device_info {
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t bus_number;
  uint8_t device_address;
  /// 通信类接口（承载控制通道 + 中断通知）。
  uint8_t control_interface;
  /// 数据类接口（承载 bulk IN/OUT）。
  uint8_t data_interface;
  /// 匹配到的接口签名，用于说明「按哪种 RNDIS 形态识别的」。
  uint8_t interface_class;
  uint8_t interface_subclass;
  uint8_t interface_protocol;
  /// 是否走了 Android quirk 的兜底路径（CDC Union 描述符不可信时）。
  bool used_android_quirk;
  /// 以下三个字符串描述符是**尽力而为**的：读它们需要 libusb_open，
  /// 而设备可能已被本机的另一个进程独占。本次读不到时会回填**本进程内**上次
  /// 成功读到的值（设备没变，名字不该因为被占用就消失）；连上次的值都没有
  /// 才是空串，此时 GUI 应回落到展示 `description`（VID:PID 形式）。
  char manufacturer[TK_USB_STRING_CAPACITY];
  char product[TK_USB_STRING_CAPACITY];
  char serial[TK_USB_STRING_CAPACITY];
  /// 形如 "Bus 020 Device 003: 18d1:4ee4"，任何情况下都可用。
  char description[64];
} tk_device_info_t;

/// 枚举当前连接的 RNDIS 设备。
///
/// @param out_devices    调用方提供的数组，可为 NULL（此时只统计数量）。
/// @param capacity       数组容量。实际找到的设备多于容量时只填前 capacity 个。
/// @param out_count      写入实际找到的设备总数（可能大于 capacity）。
/// @param read_strings   是否尝试读取厂商名 / 产品名 / 序列号。这需要
///                       libusb_open，在设备已被占用时会失败（不致命）。
///                       会话运行期间刷新列表时建议传 false —— 无论跳过还是
///                       读失败，都会回填本进程内上次成功读到的值，名字不会
///                       因此变成空串。
TK_API tk_result_t tk_list_devices(tk_device_info_t* out_devices, size_t capacity,
                                   size_t* out_count, bool read_strings, tk_error_t* out_error);

// =============================================================================
// 日志
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
  /// 墙上时间（自 Unix 纪元的纳秒）。用墙上时间而非单调时间：日志要能和系统
  /// 日志对齐。
  int64_t wall_nanos;
  char thread[TK_THREAD_NAME_CAPACITY];
  char message[TK_TEXT_CAPACITY];
} tk_log_record_t;

TK_API void tk_set_log_level(int32_t level);

/// 打开 / 关闭日志捕获。
///
/// 默认**关闭** —— 命令行程序不需要，开着白白多一份拷贝。GUI 宿主在启动时调用
/// `tk_enable_log_capture(true)`，之后周期性 `tk_drain_logs` 取走。
TK_API void tk_enable_log_capture(bool enabled);

/// 取走已缓冲的日志行（先进先出），返回实际取走的条数。
///
/// 缓冲区是定长环形队列，写满时丢弃**最旧**的记录 —— 宿主卡住时保留最新现场
/// 比保留开头更有用。被丢弃的条数通过 `out_dropped` 汇报，GUI 可据此提示
/// 「日志有缺口」。
TK_API size_t tk_drain_logs(tk_log_record_t* out_records, size_t capacity,
                            uint64_t* out_dropped);

// =============================================================================
// 会话（需要 root）
// =============================================================================

/// RNDIS 主机侧状态，与 C++ 侧 `tetherkitnext::rndis::State` 一一对应。
typedef enum tk_rndis_state {
  TK_RNDIS_UNINITIALIZED = 0,
  TK_RNDIS_INITIALIZING = 1,
  TK_RNDIS_INITIALIZED = 2,
  TK_RNDIS_DATA_INITIALIZED = 3, ///< 数据开始流动。
  TK_RNDIS_HALTING = 4,
} tk_rndis_state_t;

/// 会话的生命周期状态。这是 GUI 主界面直接绑定的那个状态。
typedef enum tk_run_state {
  TK_RUN_IDLE = 0,     ///< 已创建，尚未 start。
  TK_RUN_STARTING = 1, ///< 正在走启动序列（枚举 → 握手 → 建网卡 → 开桥接）。
  TK_RUN_RUNNING = 2,  ///< 数据路径已跑起来。
  TK_RUN_STOPPING = 3,
  TK_RUN_STOPPED = 4,
  TK_RUN_FAILED = 5, ///< 启动失败或运行中遇到不可恢复错误，原因见 status.fatal。
} tk_run_state_t;

/// 会话配置。用 `tk_session_config_init` 填好默认值后再改需要改的字段 ——
/// 直接 `{0}` 初始化会得到一堆无效的 0。
typedef struct tk_session_config {
  /// 设备筛选。全为 0 表示用第一个找到的 RNDIS 设备。
  uint16_t vendor_id;
  uint16_t product_id;
  /// 只匹配指定总线上的指定地址（两者必须同时给出）。用于区分两台同型号设备。
  uint8_t bus_number;
  uint8_t device_address;

  /// 希望使用的 MTU。设备装不下时会被协商下调；上限受 feth 的
  /// sysctl net.link.fake.max_mtu 约束。
  uint32_t mtu;
  /// 是否把系统侧网卡的 MAC 设为设备汇报的地址。默认开。
  bool adopt_device_mac;

  /// 并发在飞的 bulk IN / OUT 传输数。
  uint32_t rx_transfer_count;
  uint32_t tx_transfer_count;
  /// 每个 bulk IN 缓冲的 KiB 数。
  uint32_t rx_transfer_kib;
  /// 在 INITIALIZE_MSG 里宣称的 MaxTransferSize（KiB）。这是让设备聚合多包的
  /// 唯一手段，也是吞吐的主要杠杆。
  uint32_t max_transfer_kib;
  /// BPF 内核抓包缓冲的 KiB 数。
  uint32_t bpf_buffer_kib;
} tk_session_config_t;

/// 用推荐默认值填充配置。
TK_API void tk_session_config_init(tk_session_config_t* out_config);

/// 会话事件。
typedef enum tk_event_kind {
  /// RNDIS 状态迁移。`a` = 迁移前状态，`b` = 迁移后状态（均为 tk_rndis_state_t）。
  TK_EVENT_RNDIS_STATE = 0,
  /// 协商完成。`a` = 最终 MTU，`b` = 链路速率（Mbps）。
  TK_EVENT_NEGOTIATED = 1,
  /// 链路 up / down。`a` = 1 表示已连接。
  TK_EVENT_LINK = 2,
  /// 设备软复位。`a` = 1 表示寻址信息丢失、已重放。
  TK_EVENT_DEVICE_RESET = 3,
  /// 不可恢复错误，`text` 为原因。会话随后进入 TK_RUN_FAILED。
  TK_EVENT_FATAL = 4,
  /// 会话生命周期状态变化。`a` = 迁移前，`b` = 迁移后（均为 tk_run_state_t）。
  TK_EVENT_RUN_STATE = 5,
} tk_event_kind_t;

typedef struct tk_event {
  int32_t kind; ///< tk_event_kind_t
  int64_t wall_nanos;
  int64_t a;
  int64_t b;
  char text[TK_TEXT_CAPACITY];
} tk_event_t;

/// 数据路径与链路的完整快照。GUI 定时（建议 500 ms～1 s）拉取它刷新界面。
typedef struct tk_session_status {
  int32_t run_state;   ///< tk_run_state_t
  int32_t rndis_state; ///< tk_rndis_state_t
  bool link_up;
  bool paused; ///< 数据搬运是否处于暂停（链路 down 或设备复位期间）。

  /// 系统侧网卡名（主机在这张上配 IP）。未创建时为空串。
  char system_interface[TK_INTERFACE_NAME_CAPACITY];
  /// 驱动侧网卡名（TetherKitNext 的 BPF 挂在这张上）。仅用于排障展示。
  char driver_interface[TK_INTERFACE_NAME_CAPACITY];

  /// 设备汇报的永久 MAC。未协商时全 0。
  uint8_t device_mac[6];
  /// 协商后的最终 MTU。
  uint32_t mtu;
  /// 链路速率（Mbps）。
  uint32_t link_speed_mbps;
  /// 设备厂商描述串（来自 OID_GEN_VENDOR_DESCRIPTION）。
  char vendor_description[64];
  /// 正在使用的设备，形如 "Bus 020 Device 003: 18d1:4ee4"。
  char device_description[64];

  /// 累计计数器（RX = 设备 → 主机，TX = 主机 → 设备）。
  /// 速率由 GUI 自己按两次快照做差算，库不做窗口平滑。
  uint64_t rx_frames;
  uint64_t rx_bytes;
  uint64_t rx_dropped;
  uint64_t tx_frames;
  uint64_t tx_bytes;
  uint64_t tx_dropped;
  /// RX 队列当前深度，用于判断哪一侧是瓶颈。
  uint64_t rx_queue_depth;
  /// 内核 BPF 侧累计丢包（bs_drop）。
  uint64_t link_kernel_drops;
  /// 因 TX 传输池没有空闲槽位而产生的背压次数。
  uint64_t tx_backpressure;

  /// 采样时刻的单调纳秒计数。GUI 用它算速率，别用自己的时钟 ——
  /// 两次拉取之间的真实间隔可能被调度拉长。
  int64_t monotonic_nanos;

  /// run_state == TK_RUN_FAILED 时的原因；否则为空串。
  char fatal[TK_ERROR_MESSAGE_CAPACITY];
} tk_session_status_t;

/// 不透明会话句柄。
typedef struct tk_session tk_session_t;

/// 创建会话。**不做任何 I/O**，只是记下配置。
TK_API tk_session_t* tk_session_create(const tk_session_config_t* config, tk_error_t* out_error);

/// 启动会话。**非阻塞**：内部起一条控制线程跑启动序列与保活循环，本函数立刻返回。
///
/// 返回 TK_OK 只表示「启动请求已受理」，真正的成败要通过 `tk_session_status`
/// 的 run_state 与事件队列观察。这样设计是因为启动序列包含 USB 握手，在慢设备上
/// 可能要几百毫秒到数秒，阻塞 GUI 主线程不可接受。
///
/// 控制通道必须跑在**非 libusb 事件线程**上（同步 API 在事件线程上返回
/// LIBUSB_ERROR_BUSY），库内部保证了这一点，调用方无需关心线程。
TK_API tk_result_t tk_session_start(tk_session_t* session, tk_error_t* out_error);

/// 请求停机并等待控制线程退出。幂等。
///
/// ⚠️ **不能从事件轮询的回调里调用** —— 本函数会 join 控制线程。C ABI 这一层
/// 没有回调，所以只要调用方不在自己的事件处理里持有会引发重入的锁就没问题。
TK_API tk_result_t tk_session_stop(tk_session_t* session);

/// 停机并销毁。传 NULL 是空操作。
TK_API void tk_session_destroy(tk_session_t* session);

/// 取一份状态快照。任意线程可调。
TK_API tk_result_t tk_session_status_get(tk_session_t* session, tk_session_status_t* out_status);

/// 取走已排队的事件（先进先出），返回实际取走的条数。
///
/// 与日志一样是定长环形队列，写满丢最旧。GUI 漏掉几条状态迁移事件不影响正确性
/// —— 权威状态始终是 `tk_session_status_get` 的快照，事件只是用来做动画与提示。
TK_API size_t tk_session_poll_events(tk_session_t* session, tk_event_t* out_events,
                                     size_t capacity);

// =============================================================================
// 网卡 IP 配置（需要 root）
// =============================================================================

/// 上网方式。
typedef enum tk_ip_mode {
  /// 交给系统的 IPConfiguration 跑 DHCP。它会自动完成四件事：拿租约、配
  /// scoped DNS、装 scoped 默认路由、把服务发布到动态存储。
  TK_IP_MODE_DHCP = 0,
  /// 静态 IP。地址仍然经 IPConfiguration 下发（而不是裸 SIOCAIFADDR），
  /// 这样才能得到一个「经正规路径注册」的服务 —— 手工往动态存储写条目是
  /// **不会被 IPMonitor 采纳**的，这一点已实测确认。
  TK_IP_MODE_MANUAL = 1,
  /// 撤销配置（等价于 `ipconfig set <if> NONE`）。
  TK_IP_MODE_NONE = 2,
} tk_ip_mode_t;

typedef struct tk_ip_config {
  int32_t mode; ///< tk_ip_mode_t

  /// 以下四项仅在 mode == TK_IP_MODE_MANUAL 时使用。
  char address[TK_ADDRESS_CAPACITY];
  char netmask[TK_ADDRESS_CAPACITY];
  /// 路由器（网关）。留空表示不配任何默认路由。
  char router[TK_ADDRESS_CAPACITY];
  char dns[TK_DNS_MAX][TK_ADDRESS_CAPACITY];
  int32_t dns_count;

  /// 是否把**全局**默认路由也指向本网卡。
  ///
  /// 不开时只会有一条 scoped 默认路由（`RTF_IFSCOPE`），绑定到本接口的流量走它，
  /// 未绑定的流量仍走系统主服务。只有在同时存在更高优先级的连通服务（Wi-Fi、
  /// VPN）时才需要开 —— USB 网络共享的典型场景恰恰是没有别的网络可用，
  /// 那时本网卡自然就是主服务，这个开关不用动。
  bool set_default_route;
} tk_ip_config_t;

/// 用默认值（DHCP）填充配置。
TK_API void tk_ip_config_init(tk_ip_config_t* out_config);

/// 网卡当前**真实生效**的 IP 状态。
///
/// 刻意不复述「我们下发了什么」而是回读系统状态：静态模式下 DNS 能不能生效
/// 取决于 IPMonitor 认不认这个服务，只有回读才能给用户准确的反馈。
typedef struct tk_net_state {
  bool has_address;
  char address[TK_ADDRESS_CAPACITY];
  char netmask[TK_ADDRESS_CAPACITY];
  char router[TK_ADDRESS_CAPACITY];
  char dns[TK_DNS_MAX][TK_ADDRESS_CAPACITY];
  int32_t dns_count;
  /// IPConfiguration 汇报的配置方式（"DHCP" / "MANUAL" / "NONE" / ""）。
  char method[16];
  /// IPConfiguration 汇报的服务状态（如 "BOUND"）。
  char service_state[24];
  /// 本接口上是否存在一条默认路由（scoped 或全局）。
  bool has_default_route;
  /// 全局默认路由当前是否指向本接口。
  bool is_primary_default_route;
} tk_net_state_t;

/// 按配置给网卡配 IP。需要 root。
TK_API tk_result_t tk_net_apply(const char* interface_name, const tk_ip_config_t* config,
                                tk_error_t* out_error);

/// 撤销网卡上的 IP 配置（`ipconfig set <if> NONE`）。需要 root。
TK_API tk_result_t tk_net_clear(const char* interface_name, tk_error_t* out_error);

/// 回读网卡真实生效的 IP 状态。**不需要 root**。
TK_API tk_result_t tk_net_query(const char* interface_name, tk_net_state_t* out_state,
                                tk_error_t* out_error);

// =============================================================================
// 孤儿网卡清理（需要 root）
// =============================================================================

/// 销毁进程被强杀后残留在内核里的 feth 接口。
///
/// 为什么需要：进程被 SIGKILL 时 C++ 的析构不会跑，feth 会留在内核里；信号
/// 处理器拦不住 SIGKILL，所以只能靠**下次启动时兜底清理**。会话在创建 feth 时
/// 会把接口名记进 `/var/run/tetherkitnext-interfaces`，本函数读它并逐个销毁。
///
/// @param out_removed 写入实际销毁的接口数，可为 NULL。
TK_API tk_result_t tk_cleanup_orphan_interfaces(size_t* out_removed, tk_error_t* out_error);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // TETHERKITNEXT_CAPI_TETHERKITNEXT_C_H_
