# src —— 各功能模块
#
# 依赖方向严格单向，禁止反向依赖或循环依赖：
#
#   tk_common  ← 无依赖（日志、错误、无锁容器、内存池、统计）
#      ↑
#   tk_rndis   ← common          （RNDIS 线格式与状态机，纯逻辑，不碰 I/O）
#   tk_net     ← common          （feth 虚拟网卡 + BPF 链路，仅 macOS 系统调用）
#   tk_usb     ← common          （libusb 封装）
#      ↑
#   tk_core    ← common+rndis+net+usb （数据路径桥接与运行时编排）
#      ↑
#   tetherkit  ← core            （命令行可执行程序）
#   tk_capi    ← core            （C ABI，供 Swift GUI 使用）

# ------------------------------------------------------------------------------
# 公共编译属性：所有本项目目标都继承它
# ------------------------------------------------------------------------------
add_library(tetherkit_base INTERFACE)
target_include_directories(tetherkit_base INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(tetherkit_base INTERFACE tetherkit_warnings tetherkit_sanitizers
                                               tetherkit_optimizations)

# ------------------------------------------------------------------------------
# 版本号：从模板生成，避免版本变更导致全量重编译
# ------------------------------------------------------------------------------
# Build identity. The marketing version alone cannot tell two builds apart
# (every 0.2.0 prerelease reports "0.2.0"), so the app could not notice that
# the background daemon was still running an older 0.2.0 binary. The build ID
# makes each commit distinguishable; the app compares it with the daemon's.
# Resolved at configure time (release builds always configure fresh);
# override with -DTETHERKIT_BUILD_ID=... when building outside git.
set(TETHERKIT_BUILD_ID "" CACHE STRING "Build identifier embedded in the version info")
set(_tk_build_id "${TETHERKIT_BUILD_ID}")
if(NOT _tk_build_id)
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE _tk_build_id
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  if(NOT _tk_build_id)
    set(_tk_build_id "unknown")
  endif()
endif()
set(TETHERKIT_BUILD_ID_RESOLVED "${_tk_build_id}")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.cc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.cc"
               @ONLY)

add_library(tk_version STATIC "${CMAKE_CURRENT_BINARY_DIR}/version.cc")
target_link_libraries(tk_version PUBLIC tetherkit_base)

add_subdirectory(common)
add_subdirectory(rndis)
add_subdirectory(net)
add_subdirectory(usb)
add_subdirectory(core)
add_subdirectory(capi)
add_subdirectory(app)
