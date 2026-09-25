// C ABI 层的单元测试。
//
// 覆盖那些「写错了不会当场崩、但会在 GUI 上表现成乱码或安全漏洞」的地方：
// UTF-8 边界截断、网卡名校验、日志环形缓冲的丢弃语义。
//
// 会话与网卡配置需要 root 与真实设备，不在这里测 —— 只测它们的参数校验分支。
#include <array>
#include <cstring>
#include <string>

#include <doctest.h>

#include "capi_support.h"
#include "process_runner.h"
#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/logging.h"

using tetherkitnext::capi::CopyText;
using tetherkitnext::capi::IsValidFethName;

TEST_SUITE("capi.support") {

TEST_CASE("CopyText 正常拷贝并补终止符") {
  std::array<char, 16> buffer{};
  CopyText(buffer.data(), buffer.size(), "feth0");
  CHECK(std::string{buffer.data()} == "feth0");
}

TEST_CASE("CopyText 容量为 0 或目标为空时不写内存") {
  CopyText(nullptr, 16, "x");  // 不崩即通过
  std::array<char, 4> buffer{'a', 'b', 'c', 'd'};
  CopyText(buffer.data(), 0, "xyz");
  CHECK(buffer[0] == 'a');
}

TEST_CASE("CopyText 截断落在 UTF-8 字符边界上") {
  // 「设备」= 6 字节（每字 3 字节）。给 6 字节容量（可用 5 字节 + 终止符）时，
  // 只能装下第一个字；若按字节硬切会留下半个「备」，Swift 侧整串会变成替换字符。
  std::array<char, 6> buffer{};
  CopyText(buffer.data(), buffer.size(), "设备");
  CHECK(std::string{buffer.data()} == "设");
  CHECK(std::strlen(buffer.data()) == 3);
}

TEST_CASE("CopyText 恰好装得下时不截断") {
  std::array<char, 7> buffer{};
  CopyText(buffer.data(), buffer.size(), "设备");
  CHECK(std::string{buffer.data()} == "设备");
}

TEST_CASE("CopyText 首字符就装不下时得到空串而非半个字符") {
  std::array<char, 3> buffer{};  // 可用 2 字节，装不下 3 字节的「设」
  CopyText(buffer.data(), buffer.size(), "设备");
  CHECK(std::string{buffer.data()}.empty());
}

TEST_CASE("IsValidFethName 只接受 feth+数字") {
  CHECK(IsValidFethName("feth0"));
  CHECK(IsValidFethName("feth12"));

  SUBCASE("拒绝真实物理网卡 —— 防止误把用户的 Wi-Fi 配置冲掉") {
    CHECK_FALSE(IsValidFethName("en0"));
    CHECK_FALSE(IsValidFethName("bridge0"));
  }
  SUBCASE("拒绝缺少编号与非数字后缀") {
    CHECK_FALSE(IsValidFethName("feth"));
    CHECK_FALSE(IsValidFethName("fethX"));
    CHECK_FALSE(IsValidFethName("feth0a"));
  }
  SUBCASE("拒绝壳元字符与路径分隔符") {
    CHECK_FALSE(IsValidFethName("feth0; rm -rf /"));
    CHECK_FALSE(IsValidFethName("../feth0"));
    CHECK_FALSE(IsValidFethName(""));
  }
  SUBCASE("拒绝超过 IFNAMSIZ 的名字") {
    CHECK_FALSE(IsValidFethName("feth123456789012345"));
  }
}

namespace {

using tetherkitnext::capi::DeviceIdentity;
using tetherkitnext::capi::ReconcileDeviceStrings;
using tetherkitnext::capi::RememberedDeviceStrings;

/// 造一条已填好身份与字符串的枚举结果。字符串传空串表示「这次没读到」。
tk_device_info_t DeviceInfo(const DeviceIdentity& identity, const char* manufacturer,
                            const char* product, const char* serial) {
  tk_device_info_t info{};
  info.vendor_id = identity.vendor_id;
  info.product_id = identity.product_id;
  info.bus_number = identity.bus_number;
  info.device_address = identity.device_address;
  CopyText(info.manufacturer, manufacturer);
  CopyText(info.product, product);
  CopyText(info.serial, serial);
  return info;
}

}  // namespace

TEST_CASE("ReconcileDeviceStrings 在读不到时回填上次成功读到的名字") {
  // 场景即 GUI 上那个真实缺陷：连接后设备被独占、读不到字符串，
  // 「vivo iQOO Z10x」退化成「USB 设备 2d95:600b」。
  const DeviceIdentity phone{.bus_number = 0, .device_address = 1,
                             .vendor_id = 0x2d95, .product_id = 0x600b};
  std::vector<RememberedDeviceStrings> memory;
  const std::vector<DeviceIdentity> present{phone};

  // 第一次：空闲时读到了 → 记住。
  std::vector<tk_device_info_t> infos{DeviceInfo(phone, "vivo", "iQOO Z10x", "10AFAC2X72005KT")};
  ReconcileDeviceStrings(memory, present, infos);
  REQUIRE(memory.size() == 1);

  // 第二次：会话启动，读被跳过（全空）→ 回填。
  infos = {DeviceInfo(phone, "", "", "")};
  ReconcileDeviceStrings(memory, present, infos);
  CHECK(std::string{infos[0].manufacturer} == "vivo");
  CHECK(std::string{infos[0].product} == "iQOO Z10x");
  CHECK(std::string{infos[0].serial} == "10AFAC2X72005KT");

  SUBCASE("再次读到新值时覆盖记忆") {
    infos = {DeviceInfo(phone, "vivo", "iQOO Z10x", "NEWSERIAL")};
    ReconcileDeviceStrings(memory, present, infos);
    infos = {DeviceInfo(phone, "", "", "")};
    ReconcileDeviceStrings(memory, present, infos);
    CHECK(std::string{infos[0].serial} == "NEWSERIAL");
  }
}

TEST_CASE("ReconcileDeviceStrings 不把旧名字安给接替同一地址的另一台设备") {
  const DeviceIdentity old_phone{.bus_number = 0, .device_address = 1,
                                 .vendor_id = 0x2d95, .product_id = 0x600b};
  std::vector<RememberedDeviceStrings> memory;
  std::vector<tk_device_info_t> infos{DeviceInfo(old_phone, "vivo", "iQOO Z10x", "SN1")};
  ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{old_phone}}, infos);
  REQUIRE(memory.size() == 1);

  SUBCASE("同地址但 VID:PID 不同 → 身份不同，不回填") {
    const DeviceIdentity other{.bus_number = 0, .device_address = 1,
                               .vendor_id = 0x18d1, .product_id = 0x4ee4};
    infos = {DeviceInfo(other, "", "", "")};
    ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{other}}, infos);
    CHECK(std::string{infos[0].product}.empty());
  }

  SUBCASE("设备拔掉（不在场）→ 记忆清除；重插后读不到也不回填") {
    // 拔掉：present 为空。
    infos.clear();
    ReconcileDeviceStrings(memory, {}, infos);
    CHECK(memory.empty());

    // 重插同身份（可能已是同型号的另一台），读不到 → 宁可显示 VID:PID。
    infos = {DeviceInfo(old_phone, "", "", "")};
    ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{old_phone}}, infos);
    CHECK(std::string{infos[0].product}.empty());
  }
}

TEST_CASE("ReconcileDeviceStrings 对没有字符串的设备保持诚实的空串") {
  const DeviceIdentity mute{.bus_number = 2, .device_address = 7,
                            .vendor_id = 0x1234, .product_id = 0x5678};
  std::vector<RememberedDeviceStrings> memory;
  std::vector<tk_device_info_t> infos{DeviceInfo(mute, "", "", "")};
  ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{mute}}, infos);
  // 不记忆（没读到东西）、不回填（无中生有）。
  CHECK(memory.empty());
  CHECK(std::string{infos[0].product}.empty());
}

TEST_CASE("ReconcileDeviceStrings 不清除仍在场但没被填出的设备的记忆") {
  // 调用方数组容量不够时，present 比 infos 长 —— 超出部分只是没被填，
  // 不是拔掉了。
  const DeviceIdentity first{.bus_number = 0, .device_address = 1,
                             .vendor_id = 0x2d95, .product_id = 0x600b};
  const DeviceIdentity second{.bus_number = 0, .device_address = 2,
                              .vendor_id = 0x18d1, .product_id = 0x4ee4};
  std::vector<RememberedDeviceStrings> memory;
  const std::vector<DeviceIdentity> both{first, second};

  std::vector<tk_device_info_t> infos{DeviceInfo(first, "vivo", "iQOO Z10x", "SN1"),
                                      DeviceInfo(second, "Google", "Pixel", "SN2")};
  ReconcileDeviceStrings(memory, both, infos);
  REQUIRE(memory.size() == 2);

  // 容量降到 1：只填了第一台，第二台仍在场。
  infos = {DeviceInfo(first, "", "", "")};
  ReconcileDeviceStrings(memory, both, infos);
  CHECK(memory.size() == 2);

  // 之后第二台重新被填出且读不到时，记忆还在，能回填。
  infos = {DeviceInfo(second, "", "", "")};
  ReconcileDeviceStrings(memory, both, infos);
  CHECK(std::string{infos[0].product} == "Pixel");
}

}  // TEST_SUITE("capi.support")

TEST_SUITE("capi.basics") {

TEST_CASE("tk_version 填出非空的版本串") {
  tk_version_info_t version{};
  tk_version(&version);
  CHECK(std::strlen(version.text) > 0);
  CHECK(std::strlen(version.build) > 0);
  CHECK(std::strlen(version.libusb) > 0);
  CHECK(version.major + version.minor + version.patch > 0);

  SUBCASE("传 nullptr 是空操作而非崩溃") {
    tk_version(nullptr);
  }
}

TEST_CASE("tk_check_environment 永远成功，只在字段里表达结论") {
  tk_environment_t environment{};
  CHECK(tk_check_environment(&environment) == TK_OK);
  // 测试进程不是 root，这一条是确定的。
  CHECK_FALSE(environment.is_root);
  // sysctl 合格时不应该同时带着说明文字。
  if (environment.sysctls_ok) {
    CHECK(std::strlen(environment.sysctl_detail) == 0);
  }

  SUBCASE("传 nullptr 返回参数错误") {
    CHECK(tk_check_environment(nullptr) == TK_ERR_INVALID_ARGUMENT);
  }
}

TEST_CASE("tk_list_devices 允许只统计数量") {
  std::size_t count = 0;
  tk_error_t error{};
  // 开发机上通常没插 RNDIS 设备，这里只验证「调用成功且不写越界」，
  // 不断言具体数量。
  const tk_result_t result = tk_list_devices(nullptr, 0, &count, false, &error);
  CHECK(result == TK_OK);
  CHECK(std::strlen(error.message) == 0);

  SUBCASE("缺少 out_count 时拒绝调用") {
    CHECK(tk_list_devices(nullptr, 0, nullptr, false, nullptr) == TK_ERR_INVALID_ARGUMENT);
  }
}

TEST_CASE("重复枚举不会反复初始化 libusb") {
  // GUI 会周期性刷新设备列表。若每次枚举都新建一个 libusb 上下文，就会每次都
  // 起停一条事件线程与一条 IOKit runloop 线程，日志里还会被「libusb 已初始化」
  // 刷满 —— 这个问题真实发生过，这条用例把修复钉住。
  tk_enable_log_capture(true);
  const tetherkitnext::LogLevel saved_level = tetherkitnext::GetLogLevel();
  tetherkitnext::SetLogLevel(tetherkitnext::LogLevel::kInfo);

  std::size_t count = 0;
  std::array<tk_log_record_t, 64> records{};
  std::uint64_t dropped = 0;

  // 先枚举一次并把日志清空，确保共享上下文已经建立（第一次初始化是应该有的）。
  tk_list_devices(nullptr, 0, &count, false, nullptr);
  while (tk_drain_logs(records.data(), records.size(), &dropped) > 0) {
  }

  constexpr int kRepeats = 3;
  for (int i = 0; i < kRepeats; ++i) {
    CHECK(tk_list_devices(nullptr, 0, &count, false, nullptr) == TK_OK);
  }

  std::size_t initialization_lines = 0;
  std::size_t taken = 0;
  while ((taken = tk_drain_logs(records.data(), records.size(), &dropped)) > 0) {
    for (std::size_t i = 0; i < taken; ++i) {
      if (std::string{records[i].message}.find("libusb 已初始化") != std::string::npos) {
        ++initialization_lines;
      }
    }
  }
  CHECK(initialization_lines == 0);

  tetherkitnext::SetLogLevel(saved_level);
  tk_enable_log_capture(false);
}

}  // TEST_SUITE("capi.basics")

TEST_SUITE("capi.log_ring") {

TEST_CASE("日志捕获关闭时不产生记录") {
  tk_enable_log_capture(false);
  TETHERKITNEXT_ERROR("这条不该被捕获");

  std::array<tk_log_record_t, 4> records{};
  std::uint64_t dropped = 0;
  CHECK(tk_drain_logs(records.data(), records.size(), &dropped) == 0);
}

TEST_CASE("开启捕获后能按序取回日志") {
  tk_enable_log_capture(true);
  const tetherkitnext::LogLevel saved_level = tetherkitnext::GetLogLevel();
  tetherkitnext::SetLogLevel(tetherkitnext::LogLevel::kInfo);

  TETHERKITNEXT_INFO("第一条");
  TETHERKITNEXT_WARN("第二条");

  std::array<tk_log_record_t, 8> records{};
  std::uint64_t dropped = 0;
  const std::size_t taken = tk_drain_logs(records.data(), records.size(), &dropped);

  CHECK(taken == 2);
  CHECK(dropped == 0);
  CHECK(std::string{records[0].message} == "第一条");
  CHECK(records[0].level == TK_LOG_INFO);
  CHECK(std::string{records[1].message} == "第二条");
  CHECK(records[1].level == TK_LOG_WARN);
  CHECK(records[0].wall_nanos > 0);

  SUBCASE("取空之后再取返回 0") {
    CHECK(tk_drain_logs(records.data(), records.size(), &dropped) == 0);
  }

  tetherkitnext::SetLogLevel(saved_level);
  tk_enable_log_capture(false);
}

TEST_CASE("缓冲写满时丢最旧的并汇报丢弃数") {
  tk_enable_log_capture(true);
  const tetherkitnext::LogLevel saved_level = tetherkitnext::GetLogLevel();
  tetherkitnext::SetLogLevel(tetherkitnext::LogLevel::kInfo);

  // 容量是 256（见 log_ring.cc），多打 10 条把最旧的挤掉。
  constexpr int kOverflow = 10;
  constexpr int kTotal = 256 + kOverflow;
  for (int i = 0; i < kTotal; ++i) {
    TETHERKITNEXT_INFO("第 {} 条", i);
  }

  std::array<tk_log_record_t, 512> records{};
  std::uint64_t dropped = 0;
  const std::size_t taken = tk_drain_logs(records.data(), records.size(), &dropped);

  CHECK(taken == 256);
  CHECK(dropped == kOverflow);
  // 留下来的应该是**最新**的那 256 条，所以第一条是第 10 条。
  CHECK(std::string{records[0].message} == "第 10 条");
  CHECK(std::string{records[taken - 1].message} == "第 265 条");

  SUBCASE("丢弃计数取走后归零") {
    std::uint64_t again = 1;
    tk_drain_logs(records.data(), records.size(), &again);
    CHECK(again == 0);
  }

  tetherkitnext::SetLogLevel(saved_level);
  tk_enable_log_capture(false);
}

}  // TEST_SUITE("capi.log_ring")

TEST_SUITE("capi.session") {

TEST_CASE("tk_session_config_init 填出可直接使用的默认值") {
  tk_session_config_t config{};
  tk_session_config_init(&config);

  CHECK(config.mtu == 1500);
  CHECK(config.adopt_device_mac);
  CHECK(config.rx_transfer_count > 0);
  CHECK(config.tx_transfer_count > 0);
  CHECK(config.rx_transfer_kib > 0);
  CHECK(config.max_transfer_kib > 0);
  CHECK(config.bpf_buffer_kib > 0);
  // 设备筛选默认不限，否则 GUI 一上来就筛不到任何设备。
  CHECK(config.vendor_id == 0);
  CHECK(config.product_id == 0);
}

TEST_CASE("会话可创建、可查状态、可销毁（不需要 root）") {
  tk_session_config_t config{};
  tk_session_config_init(&config);

  tk_error_t error{};
  tk_session_t* session = tk_session_create(&config, &error);
  REQUIRE(session != nullptr);
  CHECK(std::strlen(error.message) == 0);

  SUBCASE("未启动时是 IDLE，且各字段都是干净的零值") {
    tk_session_status_t status{};
    REQUIRE(tk_session_status_get(session, &status) == TK_OK);
    CHECK(status.run_state == TK_RUN_IDLE);
    CHECK(status.rndis_state == TK_RNDIS_UNINITIALIZED);
    CHECK_FALSE(status.link_up);
    CHECK(std::strlen(status.system_interface) == 0);
    CHECK(std::strlen(status.fatal) == 0);
    CHECK(status.rx_frames == 0);
    CHECK(status.monotonic_nanos > 0);
  }

  SUBCASE("未启动时事件队列是空的") {
    std::array<tk_event_t, 8> events{};
    CHECK(tk_session_poll_events(session, events.data(), events.size()) == 0);
  }

  SUBCASE("非 root 启动返回 TK_ERR_PERMISSION 而不是笼统的失败") {
    // GUI 靠这个码区分「该弹授权」和「真的出错了」。
    CHECK(tk_session_start(session, &error) == TK_ERR_PERMISSION);
    CHECK(std::strlen(error.message) > 0);
  }

  SUBCASE("重复 stop 是幂等的") {
    CHECK(tk_session_stop(session) == TK_OK);
    CHECK(tk_session_stop(session) == TK_OK);
  }

  tk_session_destroy(session);
}

TEST_CASE("会话接口对空指针一律安全") {
  tk_error_t error{};
  CHECK(tk_session_create(nullptr, &error) == nullptr);
  CHECK(std::strlen(error.message) > 0);

  CHECK(tk_session_start(nullptr, nullptr) == TK_ERR_INVALID_ARGUMENT);
  CHECK(tk_session_stop(nullptr) == TK_ERR_INVALID_ARGUMENT);
  CHECK(tk_session_status_get(nullptr, nullptr) == TK_ERR_INVALID_ARGUMENT);
  CHECK(tk_session_poll_events(nullptr, nullptr, 4) == 0);
  tk_session_destroy(nullptr);  // 不崩即通过
  tk_session_config_init(nullptr);
}

}  // TEST_SUITE("capi.session")

TEST_SUITE("capi.process") {

TEST_CASE("RunTool 收集子进程输出并带回退出码") {
  const auto result = tetherkitnext::capi::RunTool("/bin/echo", {"你好", "世界"});
  REQUIRE(result.has_value());
  CHECK(result->exit_code == 0);
  CHECK(result->Succeeded());
  CHECK(result->output == "你好 世界\n");
}

TEST_CASE("RunTool 合并 stderr，且非零退出不算调用失败") {
  // sh -c 'echo boom >&2; exit 3'：验证 stderr 也被收进来、退出码原样带回。
  const auto result =
      tetherkitnext::capi::RunTool("/bin/sh", {"-c", "echo boom >&2; exit 3"});
  REQUIRE(result.has_value());
  CHECK_FALSE(result->Succeeded());
  CHECK(result->exit_code == 3);
  CHECK(result->output == "boom\n");
}

TEST_CASE("RunTool 读得下超过管道缓冲的大输出（不死锁）") {
  // 管道缓冲是 64 KiB。必须先读空再 waitpid，否则子进程写满就阻塞、
  // 我们等在 waitpid 上，双方僵住。这条用例就是钉住那个顺序的。
  const auto result = tetherkitnext::capi::RunTool(
      "/bin/sh", {"-c", "for i in $(seq 1 20000); do echo 0123456789; done"});
  REQUIRE(result.has_value());
  CHECK(result->exit_code == 0);
  CHECK(result->output.size() == 20000 * 11);
}

TEST_CASE("RunTool 对不存在的可执行文件返回错误而非崩溃") {
  const auto result = tetherkitnext::capi::RunTool("/nonexistent/tetherkitnext-test", {});
  CHECK_FALSE(result.has_value());
}

}  // TEST_SUITE("capi.process")

TEST_SUITE("capi.net_config") {

TEST_CASE("tk_ip_config_init 默认 DHCP 且不抢全局默认路由") {
  tk_ip_config_t config{};
  tk_ip_config_init(&config);
  CHECK(config.mode == TK_IP_MODE_DHCP);
  CHECK_FALSE(config.set_default_route);
  CHECK(config.dns_count == 0);
  CHECK(std::strlen(config.address) == 0);
}

TEST_CASE("拒绝对非 feth 网卡下手") {
  tk_ip_config_t config{};
  tk_ip_config_init(&config);
  tk_error_t error{};

  // 这是本模块最重要的一条防线：误传 en0 会把用户的 Wi-Fi 配置冲掉。
  CHECK(tk_net_apply("en0", &config, &error) == TK_ERR_INVALID_ARGUMENT);
  CHECK(std::string{error.message}.find("en0") != std::string::npos);

  CHECK(tk_net_clear("en0", &error) == TK_ERR_INVALID_ARGUMENT);
  CHECK(tk_net_query("en0", nullptr, &error) == TK_ERR_INVALID_ARGUMENT);

  tk_net_state_t state{};
  CHECK(tk_net_query("bridge0", &state, &error) == TK_ERR_INVALID_ARGUMENT);
  CHECK(tk_net_apply(nullptr, &config, &error) == TK_ERR_INVALID_ARGUMENT);
  CHECK(tk_net_apply("feth0", nullptr, &error) == TK_ERR_INVALID_ARGUMENT);
}

TEST_CASE("非 root 下写操作返回 TK_ERR_PERMISSION") {
  tk_ip_config_t config{};
  tk_ip_config_init(&config);
  tk_error_t error{};

  CHECK(tk_net_apply("feth9", &config, &error) == TK_ERR_PERMISSION);
  CHECK(std::strlen(error.message) > 0);
  CHECK(tk_net_clear("feth9", &error) == TK_ERR_PERMISSION);

  std::size_t removed = 1;
  CHECK(tk_cleanup_orphan_interfaces(&removed, &error) == TK_ERR_PERMISSION);
  CHECK(removed == 0);
}

TEST_CASE("查询不存在的 feth 网卡是成功且全空，而不是报错") {
  // GUI 在会话没起来时也会刷新网络状态，那时网卡还不存在 —— 这种情况必须是
  // 「没有地址」而不是「查询失败」，否则界面上会一直挂着一个假的错误。
  tk_net_state_t state{};
  tk_error_t error{};
  CHECK(tk_net_query("feth99", &state, &error) == TK_OK);
  CHECK_FALSE(state.has_address);
  CHECK(state.dns_count == 0);
  CHECK_FALSE(state.is_primary_default_route);
  CHECK(std::strlen(error.message) == 0);
}

}  // TEST_SUITE("capi.net_config")
