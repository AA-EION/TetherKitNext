# FindLibUSB.cmake —— 定位 libusb-1.0
#
# 优先走 pkg-config（Homebrew 安装的 libusb 自带 .pc 文件，能正确给出
# `include/libusb-1.0` 这个非常规的头文件目录）；失败时回退到手工搜索常见前缀。
#
# 输出：
#   LibUSB_FOUND        —— 是否找到
#   LibUSB_VERSION      —— 版本号字符串
#   LibUSB_INCLUDE_DIR  —— 含 libusb.h 的目录
#   LibUSB_LIBRARY      —— 库文件路径
#   LibUSB::LibUSB      —— 可直接 link 的 IMPORTED 目标

# LibUSB_ROOT (see scripts/build-libusb.sh) pins a specific build — the
# universal, @rpath-named libusb that release builds embed in TetherKit.app.
# When it is set, never fall back to pkg-config / Homebrew: silently linking
# the host's single-arch Homebrew copy would produce a bundle that only runs
# on the build machine.
if(DEFINED LibUSB_ROOT OR DEFINED ENV{LibUSB_ROOT})
  if(NOT LibUSB_ROOT)
    set(LibUSB_ROOT "$ENV{LibUSB_ROOT}")
  endif()
  find_path(
    LibUSB_INCLUDE_DIR
    NAMES libusb.h
    PATHS "${LibUSB_ROOT}/include"
    PATH_SUFFIXES libusb-1.0
    NO_DEFAULT_PATH)
  find_library(
    LibUSB_LIBRARY
    NAMES usb-1.0 libusb-1.0
    PATHS "${LibUSB_ROOT}/lib"
    NO_DEFAULT_PATH)
  if(EXISTS "${LibUSB_ROOT}/VERSION")
    file(READ "${LibUSB_ROOT}/VERSION" _libusb_version_file)
    string(REGEX MATCH "^[0-9.]+" PC_LIBUSB_VERSION "${_libusb_version_file}")
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBUSB QUIET libusb-1.0)
  endif()
endif()

find_path(
  LibUSB_INCLUDE_DIR
  NAMES libusb.h
  HINTS ${PC_LIBUSB_INCLUDEDIR} ${PC_LIBUSB_INCLUDE_DIRS}
  PATHS /opt/homebrew/opt/libusb/include /usr/local/opt/libusb/include /opt/local/include
        /usr/local/include /usr/include
  PATH_SUFFIXES libusb-1.0)

find_library(
  LibUSB_LIBRARY
  NAMES usb-1.0 libusb-1.0
  HINTS ${PC_LIBUSB_LIBDIR} ${PC_LIBUSB_LIBRARY_DIRS}
  PATHS /opt/homebrew/opt/libusb/lib /usr/local/opt/libusb/lib /opt/local/lib /usr/local/lib
        /usr/lib)

# 版本号：pkg-config 最可靠；否则从头文件的 LIBUSB_API_VERSION 推断一个下限。
if(PC_LIBUSB_VERSION)
  set(LibUSB_VERSION "${PC_LIBUSB_VERSION}")
elseif(LibUSB_INCLUDE_DIR AND EXISTS "${LibUSB_INCLUDE_DIR}/libusb.h")
  file(STRINGS "${LibUSB_INCLUDE_DIR}/libusb.h" _libusb_api_line
       REGEX "^#define[ \t]+LIBUSB_API_VERSION[ \t]+0x[0-9A-Fa-f]+")
  if(_libusb_api_line)
    string(REGEX REPLACE ".*0x([0-9A-Fa-f]+).*" "\\1" LibUSB_VERSION "${_libusb_api_line}")
    set(LibUSB_VERSION "API-0x${LibUSB_VERSION}")
  else()
    set(LibUSB_VERSION "unknown")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  LibUSB
  REQUIRED_VARS LibUSB_LIBRARY LibUSB_INCLUDE_DIR
  VERSION_VAR LibUSB_VERSION)

if(LibUSB_FOUND AND NOT TARGET LibUSB::LibUSB)
  add_library(LibUSB::LibUSB UNKNOWN IMPORTED)
  set_target_properties(
    LibUSB::LibUSB
    PROPERTIES IMPORTED_LOCATION "${LibUSB_LIBRARY}"
               INTERFACE_INCLUDE_DIRECTORIES "${LibUSB_INCLUDE_DIR}")
  # libusb 的 darwin 后端需要 IOKit 与 CoreFoundation，以及 Security（IOUSBHost 权限校验）。
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(SECURITY_FRAMEWORK Security REQUIRED)
  set_property(
    TARGET LibUSB::LibUSB
    APPEND
    PROPERTY INTERFACE_LINK_LIBRARIES "${IOKIT_FRAMEWORK}" "${COREFOUNDATION_FRAMEWORK}"
             "${SECURITY_FRAMEWORK}")
endif()

mark_as_advanced(LibUSB_INCLUDE_DIR LibUSB_LIBRARY)
