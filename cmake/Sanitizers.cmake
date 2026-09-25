# Sanitizers.cmake —— 消毒器开关
#
# 提供 INTERFACE 目标 tetherkitnext_sanitizers。
#
# 注意：
#  * ASan 与 TSan 互斥，不能同时开。
#  * TSan 是本项目最重要的消毒器 —— 无锁 SPSC 环形队列、libusb 事件线程与
#    BPF 读写线程之间的内存序正确性只能靠它验证。建议至少跑一遍：
#      cmake -B build-tsan -DTETHERKITNEXT_ENABLE_TSAN=ON && ctest --test-dir build-tsan
#  * sanitizer 会显著降低吞吐，性能基准务必在未开消毒器的 Release 构建上跑。

add_library(tetherkitnext_sanitizers INTERFACE)

if(TETHERKITNEXT_ENABLE_ASAN AND TETHERKITNEXT_ENABLE_TSAN)
  message(FATAL_ERROR "AddressSanitizer 与 ThreadSanitizer 不能同时启用。")
endif()

set(_tk_san_list "")

if(TETHERKITNEXT_ENABLE_ASAN)
  list(APPEND _tk_san_list address)
endif()

if(TETHERKITNEXT_ENABLE_TSAN)
  list(APPEND _tk_san_list thread)
endif()

if(TETHERKITNEXT_ENABLE_UBSAN)
  list(APPEND _tk_san_list undefined)
  # 手写线格式结构体上会做未对齐访问吗？不会 —— 我们全部走逐字节读取，
  # 因此 alignment 检查保持开启，用来抓真正的 bug。
endif()

if(_tk_san_list)
  list(JOIN _tk_san_list "," _tk_san_flags)
  set(TETHERKITNEXT_SANITIZER_SUMMARY "${_tk_san_flags}")
  target_compile_options(tetherkitnext_sanitizers INTERFACE -fsanitize=${_tk_san_flags}
                                                        -fno-omit-frame-pointer -g)
  target_link_options(tetherkitnext_sanitizers INTERFACE -fsanitize=${_tk_san_flags})
  message(STATUS "已启用消毒器: ${_tk_san_flags}")
else()
  set(TETHERKITNEXT_SANITIZER_SUMMARY "none")
endif()
