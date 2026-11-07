cmake_minimum_required(VERSION 3.24)

project(
  TetherKitNext
  VERSION 1.2.0
  DESCRIPTION "User-space RNDIS driver for macOS: libusb + feth + BPF"
  LANGUAGES CXX)

# ------------------------------------------------------------------------------
# Platform prerequisite check
#
# This project depends on macOS-private feth (if_fake) virtual NICs and Darwin BPF, so it cannot be cross-platform.
# ------------------------------------------------------------------------------
if(NOT APPLE)
  message(FATAL_ERROR "TetherKitNext supports macOS only: it depends on feth (if_fake) and Darwin BPF.")
endif()

# ------------------------------------------------------------------------------
# Global language standard
#
# Why C++23 (verified on Apple clang 21 / macOS 26.5):
#   * std::expected -- the main vehicle for error propagation on the initialization path, avoiding a mix of exceptions and error codes.
#   * std::byteswap -- the RNDIS wire format is little-endian, so explicit conversion is needed on other endiannesses.
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Deployment target:
#   * Not set explicitly; let CMake take the host macOS version -- otherwise it would produce the linker warning "building for
#     macOS-13.3, but linking with dylib which was built for newer version" against Homebrew's libusb
#     (built for the host version).
#   * But the lower bound of 13.3 must be validated: libc++'s std::to_chars (the underlying implementation of
#     std::format floating-point formatting) carries an availability annotation, and a lower deployment target fails to compile.
set(TETHERKITNEXT_MIN_MACOS_VERSION "13.3")

if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS
                                   TETHERKITNEXT_MIN_MACOS_VERSION)
  message(
    FATAL_ERROR
      "CMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} is too low; "
      "at least ${TETHERKITNEXT_MIN_MACOS_VERSION} is required (libc++'s std::to_chars has an availability annotation).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "RelWithDebInfo"
      CACHE STRING "Build type" FORCE)
endif()

# Always export compile_commands.json for clangd / clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable position-independent code globally.
#
# Each feature module is a static library, but they are ultimately linked into the shared library libtetherkitnext -- non-PIC object
# files cannot be linked into a dylib. PIC is already the default on macOS/clang; it is written out explicitly here so that
# "why PIC is required" is documented in the build script.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Executables and libraries are all output to build/bin and build/lib, which is convenient for scripts to reference.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ------------------------------------------------------------------------------
# Build options
# ------------------------------------------------------------------------------
option(TETHERKITNEXT_BUILD_TESTS "Build unit tests" ON)
option(TETHERKITNEXT_BUILD_BENCHMARKS "Build benchmarks" ON)
option(TETHERKITNEXT_BUILD_SHARED_LIB "Build the shared library libtetherkitnext (needed by the GUI and other language bindings)" ON)
option(TETHERKITNEXT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TETHERKITNEXT_NATIVE_ARCH "Optimize for this CPU's microarchitecture (-mcpu=native; output is not portable)" OFF)
option(TETHERKITNEXT_ENABLE_LTO "Enable link-time optimization (Release-type builds)" OFF)
option(TETHERKITNEXT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(TETHERKITNEXT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(TETHERKITNEXT_ENABLE_TSAN "Enable ThreadSanitizer (validates the lock-free queues and threading model)" OFF)

include(CompilerWarnings)
include(Sanitizers)
include(Optimizations)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
find_package(LibUSB REQUIRED)

# ------------------------------------------------------------------------------
# Subdirectories
#
# Dependency direction is strictly one-way: common <- rndis/net <- usb <- core <- app
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
# Graphical interface
#
# The Swift part is built by SwiftPM (see gui/Package.swift for why CMake's Swift support is not used); here we only
# provide a convenient entry point that chains "build C++ first, then Swift":
#
#   cmake --build build --target gui
#
# Deliberately made a **manually triggered** target (the effect of EXCLUDE_FROM_ALL): the GUI needs the Xcode toolchain,
# and people who only want the command line should not be forced to install it.
# ------------------------------------------------------------------------------
if(TETHERKITNEXT_BUILD_SHARED_LIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/Package.swift")
  # Explicitly pass the current build directory to the script (it defaults to <repo>/build), so libtetherkitnext is also
  # found correctly in non-default directories such as build-rel / build-tsan.
  add_custom_target(
    gui
    COMMAND "${CMAKE_COMMAND}" -E env "TETHERKITNEXT_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "LibUSB_ROOT=${LibUSB_ROOT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/gui/Scripts/build-gui.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS tetherkitnext_shared tetherkitnext
    COMMENT "Building TetherKitNext.app and tetherkitnext-helper"
    USES_TERMINAL
    VERBATIM)
  set_property(TARGET gui APPEND PROPERTY
               ADDITIONAL_CLEAN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist")
endif()

# ------------------------------------------------------------------------------
# Build configuration summary
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== TetherKitNext ${PROJECT_VERSION} build configuration ====")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard      : C++${CMAKE_CXX_STANDARD}")
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  set(_tk_deploy "macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
  set(_tk_deploy "host default")
endif()
message(STATUS "  Deployment target : ${_tk_deploy} (minimum ${TETHERKITNEXT_MIN_MACOS_VERSION})")
message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  libusb            : ${LibUSB_VERSION} (${LibUSB_LIBRARY})")
message(STATUS "  Unit tests        : ${TETHERKITNEXT_BUILD_TESTS}")
message(STATUS "  Benchmarks        : ${TETHERKITNEXT_BUILD_BENCHMARKS}")
message(STATUS "  Native -mcpu      : ${TETHERKITNEXT_NATIVE_ARCH}")
message(STATUS "  LTO               : ${TETHERKITNEXT_ENABLE_LTO}")
message(STATUS "  ASan/UBSan/TSan   : ${TETHERKITNEXT_ENABLE_ASAN}/${TETHERKITNEXT_ENABLE_UBSAN}/${TETHERKITNEXT_ENABLE_TSAN}")
message(STATUS "=========================================")
message(STATUS "")
