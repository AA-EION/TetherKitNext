cmake_minimum_required(VERSION 3.24)

project(
  TetherKitNext
  VERSION 1.1.1
  DESCRIPTION "macOS 用户态 RNDIS 驱动：libusb + feth + BPF"
  LANGUAGES CXX)

# ------------------------------------------------------------------------------
# 平台前置检查
#
# 本项目依赖 macOS 私有的 feth(if_fake) 虚拟网卡与 Darwin BPF，无法跨平台。
# ------------------------------------------------------------------------------
if(NOT APPLE)
  message(FATAL_ERROR "TetherKitNext 仅支持 macOS：依赖 feth(if_fake) 与 Darwin BPF。")
endif()

# ------------------------------------------------------------------------------
# 全局语言标准
#
# 选 C++23 的理由（实测于 Apple clang 21 / macOS 26.5）：
#   * std::expected —— 初始化路径的错误传播主力，避免异常与错误码混用。
#   * std::byteswap —— RNDIS 线格式是小端，跨端序时需要显式转换。
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 部署目标：
#   * 不主动设置，让 CMake 取宿主 macOS 版本 —— 否则会和 Homebrew 的 libusb
#     （按宿主版本构建）产生 "building for macOS-13.3, but linking with dylib
#     which was built for newer version" 链接告警。
#   * 但必须校验下限 13.3：libc++ 的 std::to_chars（std::format 浮点格式化的
#     底层实现）带 availability 标注，更低的部署目标会直接编译失败。
set(TETHERKITNEXT_MIN_MACOS_VERSION "13.3")

if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS
                                   TETHERKITNEXT_MIN_MACOS_VERSION)
  message(
    FATAL_ERROR
      "CMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} 过低，"
      "至少需要 ${TETHERKITNEXT_MIN_MACOS_VERSION}（libc++ 的 std::to_chars 带 availability 标注）。")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "RelWithDebInfo"
      CACHE STRING "构建类型" FORCE)
endif()

# 始终导出 compile_commands.json，供 clangd / clang-tidy 使用。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 全局开启位置无关代码。
#
# 各功能模块都是静态库，但它们最终要被链进共享库 libtetherkitnext —— 非 PIC 的目标
# 文件链不进 dylib。macOS/clang 上 PIC 本来就是默认，这里显式写出来是为了让
# 「为什么必须 PIC」这件事在构建脚本里有据可查。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 可执行文件与库统一输出到 build/bin、build/lib，方便脚本引用。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ------------------------------------------------------------------------------
# 构建选项
# ------------------------------------------------------------------------------
option(TETHERKITNEXT_BUILD_TESTS "构建单元测试" ON)
option(TETHERKITNEXT_BUILD_BENCHMARKS "构建性能基准" ON)
option(TETHERKITNEXT_BUILD_SHARED_LIB "构建共享库 libtetherkitnext（GUI 与其它语言绑定需要）" ON)
option(TETHERKITNEXT_WARNINGS_AS_ERRORS "把编译警告当作错误" OFF)
option(TETHERKITNEXT_NATIVE_ARCH "为本机 CPU 微架构优化（-mcpu=native，产物不可移植）" OFF)
option(TETHERKITNEXT_ENABLE_LTO "启用链接时优化（Release 类构建）" OFF)
option(TETHERKITNEXT_ENABLE_ASAN "启用 AddressSanitizer" OFF)
option(TETHERKITNEXT_ENABLE_UBSAN "启用 UndefinedBehaviorSanitizer" OFF)
option(TETHERKITNEXT_ENABLE_TSAN "启用 ThreadSanitizer（验证无锁队列与线程模型）" OFF)

include(CompilerWarnings)
include(Sanitizers)
include(Optimizations)

# ------------------------------------------------------------------------------
# 依赖
# ------------------------------------------------------------------------------
find_package(LibUSB REQUIRED)

# ------------------------------------------------------------------------------
# 子目录
#
# 依赖方向严格单向：common <- rndis/net <- usb <- core <- app
# ------------------------------------------------------------------------------
add_subdirectory(third_party)
add_subdirectory(src)

if(TETHERKITNEXT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TETHERKITNEXT_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ------------------------------------------------------------------------------
# 图形界面
#
# Swift 部分由 SwiftPM 构建（见 gui/Package.swift 里为什么不用 CMake 的 Swift
# 支持），这里只提供一个方便的入口，把「先建 C++ 再建 Swift」串起来：
#
#   cmake --build build --target gui
#
# 刻意做成**手动触发**的目标（EXCLUDE_FROM_ALL 的效果）：GUI 需要 Xcode 工具链，
# 而只想要命令行的人不该被迫装它。
# ------------------------------------------------------------------------------
if(TETHERKITNEXT_BUILD_SHARED_LIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/Package.swift")
  # 显式把当前构建目录传给脚本（它默认找 <repo>/build），这样在
  # build-rel / build-tsan 之类的非默认目录里也能正确找到 libtetherkitnext。
  add_custom_target(
    gui
    COMMAND "${CMAKE_COMMAND}" -E env "TETHERKITNEXT_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "LibUSB_ROOT=${LibUSB_ROOT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/gui/Scripts/build-gui.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS tetherkitnext_shared tetherkitnext
    COMMENT "构建 TetherKitNext.app 与 tetherkitnext-helper"
    USES_TERMINAL
    VERBATIM)
  set_property(TARGET gui APPEND PROPERTY
               ADDITIONAL_CLEAN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist")
endif()

# ------------------------------------------------------------------------------
# 构建配置摘要
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== TetherKitNext ${PROJECT_VERSION} 构建配置 ====")
message(STATUS "  构建类型          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准          : C++${CMAKE_CXX_STANDARD}")
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  set(_tk_deploy "macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
  set(_tk_deploy "宿主默认")
endif()
message(STATUS "  部署目标          : ${_tk_deploy}（下限 ${TETHERKITNEXT_MIN_MACOS_VERSION}）")
message(STATUS "  编译器            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  libusb            : ${LibUSB_VERSION} (${LibUSB_LIBRARY})")
message(STATUS "  单元测试          : ${TETHERKITNEXT_BUILD_TESTS}")
message(STATUS "  性能基准          : ${TETHERKITNEXT_BUILD_BENCHMARKS}")
message(STATUS "  本机微架构优化    : ${TETHERKITNEXT_NATIVE_ARCH}")
message(STATUS "  LTO               : ${TETHERKITNEXT_ENABLE_LTO}")
message(STATUS "  ASan/UBSan/TSan   : ${TETHERKITNEXT_ENABLE_ASAN}/${TETHERKITNEXT_ENABLE_UBSAN}/${TETHERKITNEXT_ENABLE_TSAN}")
message(STATUS "=========================================")
message(STATUS "")
