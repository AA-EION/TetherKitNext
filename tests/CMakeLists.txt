# tests —— 单元测试
#
# 全部测试链接进单个可执行文件 tetherkitnext_tests，理由：
#  * doctest 的注册是全局的，单一二进制启动开销最低；
#  * 通过 doctest 的 --test-suite 过滤即可按模块运行；
#  * CTest 侧按 test-suite 注册多个用例，失败定位仍然精确。

add_executable(
  tetherkitnext_tests
  test_main.cc
  test_version.cc
  test_common_byte_order.cc
  test_common_error.cc
  test_common_frame_ring.cc
  test_common_i18n.cc
  test_common_spsc_ring.cc
  test_common_stats.cc
  test_rndis_messages.cc
  test_rndis_packet_codec.cc
  test_rndis_state_machine.cc
  test_net_link.cc
  test_core_bridge.cc
  test_capi.cc)

# tk_rndis / tk_net 都 PUBLIC 链接了 tk_common，无需重复列出（否则链接器报
# "ignoring duplicate libraries"）。
# tk_core PUBLIC 链接了 tk_common/tk_rndis/tk_net/tk_usb，列它一个就够。
# tk_capi PUBLIC 链接了 tk_version 与 tk_core，因此这两个都不用重复列出。
target_link_libraries(tetherkitnext_tests PRIVATE tetherkitnext_base doctest tk_capi)
# src/capi 目录：capi_support.h 是实现内部头，不对外安装，但要能被测试直接覆盖。
target_include_directories(tetherkitnext_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                   "${PROJECT_SOURCE_DIR}/src/capi")

# ------------------------------------------------------------------------------
# CTest 注册
#
# ⚠️ 这里踩过一个「整套测试静默失效却全绿」的坑，务必注意两点：
#
#  1. **过滤参数不能加引号。** 写成 `--test-suite="${_suite}"` 时，CMake 会把
#     引号当作参数内容原样传下去（实测 ctest -V 显示传的是
#     `"--test-suite="rndis.state_machine""`），doctest 匹配不到任何用例 ——
#     于是「跑了 0 个用例」，而 doctest 对零匹配返回退出码 0，ctest 就报
#     「Passed」。真实的失败被完全吞掉。
#
#  2. 因此再加一道保险：给每个用例设 **FAIL_REGULAR_EXPRESSION**，只要输出里
#     出现「assertions: 0」就判失败。将来谁再打错 suite 名字会立刻红掉。
#     这里**必须**用 FAIL_ 而不是 PASS_REGULAR_EXPRESSION —— 后者会
#     **取代**退出码判定（CMake 的文档行为），一设上去真实的断言失败反而被
#     忽略掉，等于把刚修好的坑又挖回来。FAIL_ 是叠加在退出码之上的。
#
# 另外单独注册一个 `all` 用例跑整个二进制作为**权威结果** —— 即使某个 suite
# 忘了加进下面的列表，它也会把失败暴露出来。
# ------------------------------------------------------------------------------
add_test(NAME all COMMAND tetherkitnext_tests)

set(TETHERKITNEXT_TEST_SUITES
    version
    common.byte_order
    common.error
    common.frame_ring
    common.frame_ring_batch
    common.i18n
    common.spsc_ring
    common.stats
    rndis.messages
    rndis.packet_codec
    rndis.state_machine
    rndis.state_machine_timeout
    net.abi
    net.loopback
    net.feth
    core.bridge
    capi.support
    capi.basics
    capi.log_ring
    capi.session
    capi.process
    capi.net_config)

foreach(_suite IN LISTS TETHERKITNEXT_TEST_SUITES)
  # 注意：--test-suite= 后面**不要**加引号，见上方说明。
  add_test(NAME "${_suite}" COMMAND tetherkitnext_tests --test-suite=${_suite})
  set_tests_properties("${_suite}" PROPERTIES FAIL_REGULAR_EXPRESSION "assertions: +0 \\|")
endforeach()
