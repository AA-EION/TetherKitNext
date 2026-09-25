// 运行时编排：把 USB、RNDIS 状态机、feth、BPF、桥接组装成一个可运行的驱动。
//
// ★ 启动顺序（每一步都依赖前一步的产物，不能调换）★
//
//   1. 权限检查            —— 提前给出人话错误，而不是让后面一堆 EPERM 冒出来
//   2. libusb 上下文 + 事件线程
//   3. 发现并打开 RNDIS 设备（声明两个接口、解析端点）
//   4. RNDIS 控制通道 + 状态机 Start()
//        └→ 这一步才拿到：设备 MAC、协商出的 MTU、聚合上限、对齐要求
//   5. 用第 4 步的结果创建 feth 网卡对（系统侧 MAC = 设备汇报的 MAC）
//   6. 在驱动侧接口上打开 BPF
//   7. 用第 4 步的协商参数创建 USB 数据通道
//   8. 启动桥接层
//   9. 控制线程进入循环：定期 Poll()（保活 + 排空设备推送）
//
//   为什么 feth 必须在 RNDIS 协商**之后**创建：系统侧网卡的 MAC 要设成设备
//   汇报的 OID_802_3_PERMANENT_ADDRESS，而 MTU 要用协商结果 —— 这两个值在第 4 步
//   之前都拿不到。而 MAC 又必须在接口 IFF_UP 之前设好。
//
// ★ 停机顺序是启动顺序的严格逆序 ★
//
//   桥接层 → 数据通道（等在飞传输回收）→ BPF → feth → 状态机 HALT → 设备 → libusb
//
//   其中「数据通道等在飞传输回收」这一步必须在销毁设备**之前**完成，
//   否则就是 use-after-free（详见 usb/data_channel.h 的说明）。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/core/bridge.h"
#include "tetherkitnext/net/bpf_link.h"
#include "tetherkitnext/net/feth_device.h"
#include "tetherkitnext/rndis/state_machine.h"
#include "tetherkitnext/usb/context.h"
#include "tetherkitnext/usb/data_channel.h"
#include "tetherkitnext/usb/device.h"

namespace tetherkitnext::core {

/// 运行时的生命周期状态。这是宿主（命令行 / GUI）唯一需要关心的状态维度。
enum class RunState : std::uint8_t {
  kIdle,      ///< 已创建，尚未 Start()。
  kStarting,  ///< 控制线程正在走启动序列（枚举 → 握手 → 建网卡 → 开桥接）。
  kRunning,   ///< 数据路径已跑起来。
  kStopping,  ///< 正在按逆序拆除。
  kStopped,   ///< 已正常停机。
  kFailed,    ///< 启动失败或运行中遇到不可恢复错误，原因见 RuntimeSnapshot::fatal_message。
};

[[nodiscard]] std::string_view RunStateName(RunState state) noexcept;

/// 运行时对外通报的一条事件。
///
/// 用「事件 + 宿主自己排队」而不是让宿主直接实现 StateMachineObserver：
/// 后者的五个回调语义各异、参数是 C++ 引用，跨语言宿主没法用；而且宿主在回调里
/// 很容易不小心调回运行时（比如「收到致命错误就停机」），那是自等死锁。
struct RuntimeEvent {
  enum class Kind : std::uint8_t {
    kRndisState,   ///< a = 迁移前 rndis::State，b = 迁移后。
    kNegotiated,   ///< a = 最终 MTU，b = 链路速率（Mbps）。
    kLink,         ///< a = 1 表示链路已连接。
    kDeviceReset,  ///< a = 1 表示寻址信息丢失、已重放。
    kFatal,        ///< text = 不可恢复错误的原因。
    kRunState,     ///< a = 迁移前 RunState，b = 迁移后。
  };

  Kind kind = Kind::kRunState;
  std::int64_t a = 0;
  std::int64_t b = 0;
  std::string text;
};

/// 事件汇：宿主实现它来接收运行时事件。
///
/// ★ 两条硬约束 ★
///   1. 回调**永远在控制线程上**发生（所有事件源都在那条线程），但宿主自己的
///      线程可能同时在调 Snapshot() —— 实现里该加的锁一个都不能省。
///   2. **实现里绝不能调用 Runtime 的任何方法。** Stop() 要 join 控制线程，
///      从事件里调它就是自等死锁。正确做法是把事件排进队列，由宿主线程处理。
class RuntimeEventSink {
 public:
  RuntimeEventSink() = default;
  RuntimeEventSink(const RuntimeEventSink&) = delete;
  RuntimeEventSink& operator=(const RuntimeEventSink&) = delete;
  RuntimeEventSink(RuntimeEventSink&&) = delete;
  RuntimeEventSink& operator=(RuntimeEventSink&&) = delete;
  virtual ~RuntimeEventSink() = default;

  virtual void OnRuntimeEvent(const RuntimeEvent& event) = 0;
};

/// 运行时的完整可观测快照。
///
/// 为什么整块拷贝而不是提供一堆逐项访问器：宿主需要的是**一致的**一组数值
/// （状态与网卡名不匹配的界面很难看懂），逐项读会读到撕裂的组合。一次拷贝
/// 几百字节，对 2 Hz 的刷新频率完全不是问题。
struct RuntimeSnapshot {
  RunState run_state = RunState::kIdle;
  rndis::State rndis_state = rndis::State::kUninitialized;
  bool link_up = false;
  /// 数据搬运是否处于暂停（链路 down 或设备软复位期间）。
  bool paused = false;

  /// 系统侧网卡名（主机在这张上配 IP）。未创建时为空。
  std::string system_interface;
  /// 驱动侧网卡名（BPF 挂在这张上）。仅用于排障展示。
  std::string driver_interface;
  /// 形如 "Bus 020 Device 003: 18d1:4ee4"。未打开设备时为空。
  std::string device_description;

  rndis::DeviceInfo device_info;
  rndis::NegotiatedParameters parameters;
  BridgeStats bridge;

  /// run_state == kFailed 时的原因；否则为空。
  std::string fatal_message;
};

/// 运行时配置。默认值都在各自字段的注释里说明了推导依据。
struct RuntimeConfig {
  /// 设备筛选。全为 0 表示用第一个找到的 RNDIS 设备。
  usb::DeviceFilter device_filter;

  /// 希望使用的 MTU。设备装不下时会被协商下调。
  ///
  /// 上限受 feth 的 sysctl net.link.fake.max_mtu 约束（本机 2048）。
  std::uint32_t mtu = rndis::kDefaultMtu;

  /// 指定 feth 接口名（如 "feth7"）；留空表示让内核自动选编号。
  std::string system_interface_name;
  std::string driver_interface_name;

  /// 是否把系统侧网卡的 MAC 设为设备汇报的地址。
  ///
  /// 默认开：RNDIS 语义下设备就是这块网卡，对端的 ARP 表与 DHCP 租约都按这个
  /// MAC 建立。关掉只在排查 MAC 冲突时有用。
  bool adopt_device_mac = true;

  rndis::StateMachineConfig rndis;
  usb::DataChannelConfig data_channel;
  net::BpfConfig bpf;
  BridgeConfig bridge;

  /// 统计报告周期（毫秒）；0 表示不报告。
  std::uint32_t stats_interval_millis = 5'000;

  /// 事件汇。为 nullptr 表示不需要事件通报（命令行就不需要，它看日志）。
  ///
  /// 生命周期由调用方负责，必须活得比 Runtime 久。
  RuntimeEventSink* event_sink = nullptr;
};

/// 组装并运行整个驱动。
///
/// ★ 线程模型：Start() 是**非阻塞**的 ★
///
///   Runtime 自己拥有一条控制线程，启动序列、保活循环、停机拆除**全部**在它
///   上面跑。宿主线程只负责发号施令与读快照。
///
///   为什么必须这样：
///     * 启动序列包含 USB 握手，慢设备上要几百毫秒到数秒，阻塞 GUI 主线程不行；
///     * 控制通道用 libusb 的同步 API，而同步 API 在 libusb 事件线程上会返回
///       LIBUSB_ERROR_BUSY —— 由 Runtime 自己建线程，就不用再要求宿主「必须
///       从某个特定线程调用」这种极易违反的约定；
///     * 拆除顺序里夹着 RNDIS 的优雅停机（要发控制消息），把它也放在同一条
///       线程上，`StateMachine` 的「所有方法同线程调用」约束就自然满足了。
///
/// 用法：
/// ```
/// TETHERKITNEXT_ASSIGN_OR_RETURN(auto runtime, Runtime::Create(config));
/// TETHERKITNEXT_RETURN_IF_ERROR(runtime->Start());   // 立刻返回
/// runtime->WaitUntilStopped();                   // 命令行：把主线程挂起
/// runtime->Stop();                               // 幂等
/// ```
/// GUI 则不调 WaitUntilStopped，而是定时 Snapshot() 刷界面。
class Runtime final : public rndis::StateMachineObserver {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Runtime>> Create(const RuntimeConfig& config);

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  ~Runtime() override;

  /// 起控制线程并立刻返回。
  ///
  /// 返回成功只表示「启动请求已受理」，真正的成败要看 Snapshot().run_state。
  /// 唯一会**同步**返回失败的是 root 检查 —— 它不需要 I/O，且提前报出来能让
  /// 命令行给出即时的人话提示。
  [[nodiscard]] Status Start();

  /// 阻塞直到运行时停止（正常停机或致命错误）。命令行用它把主线程挂起。
  void WaitUntilStopped();

  /// 请求停机。可从信号处理器或任意线程调用，异步信号安全（只写一个原子）。
  ///
  /// 代价是控制线程最多要过一个循环周期（≤250 ms）才看得到。要立刻停就用
  /// Stop()，它会额外唤醒控制线程 —— 但 Stop() 会加锁，**不是**异步信号安全的。
  void RequestStop() noexcept { stop_requested_.store(true, std::memory_order_release); }

  /// 请求停机并等控制线程完成全部拆除。幂等。
  ///
  /// ⚠️ 不可从事件汇（RuntimeEventSink）里调用 —— 那是控制线程自己，会自等死锁。
  void Stop();

  /// 取一份一致的状态快照。**任意线程可调**。
  [[nodiscard]] RuntimeSnapshot Snapshot() const;

  /// 系统侧网卡名，用于给用户打印「接下来该做什么」。
  [[nodiscard]] std::string SystemInterfaceName() const;

  // ---- StateMachineObserver（均在控制线程上被调用）----
  void OnStateChanged(rndis::State from, rndis::State to) override;
  void OnNegotiated(const rndis::NegotiatedParameters& parameters,
                    const rndis::DeviceInfo& info) override;
  void OnLinkStateChanged(bool connected) override;
  void OnDeviceReset(bool addressing_lost) override;
  void OnFatalError(const Error& error) override;

 private:
  Runtime() = default;

  /// 控制线程主体：启动序列 → 控制循环 → 拆除。
  void RunControlThread() noexcept;

  /// 完整的启动序列（原 Start() 的主体）。只在控制线程上跑。
  [[nodiscard]] Status RunStartSequence();

  /// 保活 + 统计 + 停机检查的循环。只在控制线程上跑。
  void RunControlLoop();

  /// 按启动顺序的严格逆序拆除。只在控制线程上跑。
  void Teardown();

  /// 迁移生命周期状态并通报事件。
  void SetRunState(RunState next);

  /// 记录致命错误：写进快照、置 kFailed 标志、通报事件。
  void RecordFatal(const Error& error);

  /// 把各组件的当前状况刷进快照。只在控制线程上调用。
  void RefreshSnapshot();

  /// 把一条事件交给宿主。sink 为空时是空操作。
  void Emit(const RuntimeEvent& event) const;

  /// 打印「网卡已就绪，接下来该怎么配 IP」的提示。
  void PrintNextSteps() const;

  RuntimeConfig config_;

  // ---- 以下组件全部**只由控制线程**创建、访问与销毁 ----
  //
  // 宿主线程一律通过 snapshot_ 读状态，绝不碰这些指针 —— 否则 Stop() 里的
  // reset() 与宿主的读会构成数据竞争，而那种竞争在真机上表现为随机崩溃。
  std::unique_ptr<usb::Context> usb_context_;
  std::unique_ptr<usb::Device> device_;
  std::unique_ptr<usb::UsbControlChannel> control_channel_;
  std::unique_ptr<rndis::StateMachine> state_machine_;
  std::unique_ptr<net::FethPair> feth_pair_;
  std::unique_ptr<net::BpfLink> bpf_link_;
  std::unique_ptr<usb::UsbDataChannel> data_channel_;
  std::unique_ptr<Bridge> bridge_;

  std::thread control_thread_;

  /// 保护 snapshot_。读侧是宿主线程（GUI 每 500 ms 一次），写侧是控制线程，
  /// 竞争极轻，普通互斥锁足够。
  mutable std::mutex snapshot_mutex_;
  RuntimeSnapshot snapshot_;

  /// 让 Stop() 能立刻唤醒正在睡的控制线程，而不用等满一个循环周期。
  std::mutex stop_mutex_;
  std::condition_variable stop_condition_;

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> fatal_error_{false};

  /// 只在宿主线程上读写（Start / Stop / 析构），不需要同步。
  bool started_ = false;
  bool stopped_ = false;
};

}  // namespace tetherkitnext::core
