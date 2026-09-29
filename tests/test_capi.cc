// Unit tests of the C ABI layer.
//
// Covers the places where "a mistake does not crash on the spot, but shows up on the GUI as garbled text or a security hole":
// UTF-8 boundary truncation, NIC name validation, the drop semantics of the log ring buffer.
//
// Sessions and NIC configuration need root and a real device, so they are not tested here -- only their argument-validation branches are tested.
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
  CopyText(nullptr, 16, "x");  // passing if it does not crash
  std::array<char, 4> buffer{'a', 'b', 'c', 'd'};
  CopyText(buffer.data(), 0, "xyz");
  CHECK(buffer[0] == 'a');
}

TEST_CASE("CopyText 截断落在 UTF-8 字符边界上") {
  // "设备" ("device") = 6 bytes (3 bytes per character). With a capacity of 6 bytes (5 usable + the terminator),
  // only the first character fits; cutting hard by bytes would leave half of "备", and on the Swift side the whole string would become replacement characters.
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
  std::array<char, 3> buffer{};  // 2 usable bytes, cannot fit the 3-byte "设"
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

/// Builds an enumeration result with identity and strings already filled in. Passing an empty string for a string means "not read this time".
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
  // The scenario is that real defect on the GUI: after connecting the device is held exclusively and strings cannot be read,
  // and "vivo iQOO Z10x" degrades into "USB device 2d95:600b".
  const DeviceIdentity phone{.bus_number = 0, .device_address = 1,
                             .vendor_id = 0x2d95, .product_id = 0x600b};
  std::vector<RememberedDeviceStrings> memory;
  const std::vector<DeviceIdentity> present{phone};

  // First time: read while idle -> remember.
  std::vector<tk_device_info_t> infos{DeviceInfo(phone, "vivo", "iQOO Z10x", "10AFAC2X72005KT")};
  ReconcileDeviceStrings(memory, present, infos);
  REQUIRE(memory.size() == 1);

  // Second time: the session starts, the read is skipped (all empty) -> backfill.
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
    // Unplugged: present is empty.
    infos.clear();
    ReconcileDeviceStrings(memory, {}, infos);
    CHECK(memory.empty());

    // Re-plugged with the same identity (possibly already another device of the same model), cannot be read -> better to show VID:PID.
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
  // Do not remember (nothing was read), do not backfill (nothing conjured out of thin air).
  CHECK(memory.empty());
  CHECK(std::string{infos[0].product}.empty());
}

TEST_CASE("ReconcileDeviceStrings 不清除仍在场但没被填出的设备的记忆") {
  // When the caller's array capacity is insufficient, present is longer than infos -- the excess was merely not filled,
  // not unplugged.
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

  // Capacity dropped to 1: only the first was filled, and the second is still present.
  infos = {DeviceInfo(first, "", "", "")};
  ReconcileDeviceStrings(memory, both, infos);
  CHECK(memory.size() == 2);

  // Afterwards, when the second is filled again and cannot be read, the memory is still there and can backfill.
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
  // The test process is not root, so this one is certain.
  CHECK_FALSE(environment.is_root);
  // When the sysctls are acceptable there should not be explanatory text at the same time.
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
  // The development machine usually has no RNDIS device plugged in, so here we only verify "the call succeeds and does not write out of bounds",
  // without asserting a specific count.
  const tk_result_t result = tk_list_devices(nullptr, 0, &count, false, &error);
  CHECK(result == TK_OK);
  CHECK(std::strlen(error.message) == 0);

  SUBCASE("缺少 out_count 时拒绝调用") {
    CHECK(tk_list_devices(nullptr, 0, nullptr, false, nullptr) == TK_ERR_INVALID_ARGUMENT);
  }
}

TEST_CASE("重复枚举不会反复初始化 libusb") {
  // The GUI refreshes the device list periodically. If every enumeration created a new libusb context, every time it would
  // start and stop one event thread and one IOKit runloop thread, and the log would be flooded with "libusb initialized"
  // -- this problem really happened, and this test case pins the fix.
  tk_enable_log_capture(true);
  const tetherkitnext::LogLevel saved_level = tetherkitnext::GetLogLevel();
  tetherkitnext::SetLogLevel(tetherkitnext::LogLevel::kInfo);

  std::size_t count = 0;
  std::array<tk_log_record_t, 64> records{};
  std::uint64_t dropped = 0;

  // Enumerate once first and clear the log, making sure the shared context has been established (the first initialization is supposed to happen).
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

  // Capacity is 256 (see log_ring.cc); log 10 more to push out the oldest.
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
  // What remains should be the **newest** 256, so the first one is number 10.
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
  // The device filter defaults to unrestricted, otherwise the GUI could not filter any device right from the start.
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
    // The GUI relies on this code to distinguish "should pop up authorization" from "a real error".
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
  tk_session_destroy(nullptr);  // passing if it does not crash
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
  // sh -c 'echo boom >&2; exit 3': verifies that stderr is also collected and the exit code is carried back as-is.
  const auto result =
      tetherkitnext::capi::RunTool("/bin/sh", {"-c", "echo boom >&2; exit 3"});
  REQUIRE(result.has_value());
  CHECK_FALSE(result->Succeeded());
  CHECK(result->exit_code == 3);
  CHECK(result->output == "boom\n");
}

TEST_CASE("RunTool 读得下超过管道缓冲的大输出（不死锁）") {
  // The pipe buffer is 64 KiB. It must be drained first and then waitpid, otherwise once the child fills it it blocks,
  // and we wait on waitpid, and both sides freeze. This test case pins that order.
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

  // This is the most important line of defense of this module: mistakenly passing en0 would wipe out the user's Wi-Fi configuration.
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
  // The GUI also refreshes network state when a session has not started, and the NIC does not exist then -- this case must be
  // "no address" rather than "query failed", otherwise a false error would hang on the UI forever.
  tk_net_state_t state{};
  tk_error_t error{};
  CHECK(tk_net_query("feth99", &state, &error) == TK_OK);
  CHECK_FALSE(state.has_address);
  CHECK(state.dns_count == 0);
  CHECK_FALSE(state.is_primary_default_route);
  CHECK(std::strlen(error.message) == 0);
}

}  // TEST_SUITE("capi.net_config")
