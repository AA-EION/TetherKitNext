# src -- feature modules
#
# The dependency direction is strictly one-way; reverse or circular dependencies are forbidden:
#
#   tk_common  <- no dependencies (logging, errors, lock-free containers, memory pool, statistics)
#      ↑
#   tk_rndis   <- common          (RNDIS wire format and state machine; pure logic, no I/O)
#   tk_net     <- common          (feth virtual NIC + BPF link; macOS system calls only)
#   tk_usb     <- common          (libusb wrapper)
#      ↑
#   tk_core    <- common+rndis+net+usb (data-path bridge and runtime orchestration)
#      ↑
#   tetherkitnext  <- core            (command-line executable)
#   tk_capi    <- core            (C ABI, for use by the Swift GUI)

# ------------------------------------------------------------------------------
# Common compile properties: all of this project's targets inherit it
# ------------------------------------------------------------------------------
add_library(tetherkitnext_base INTERFACE)
target_include_directories(tetherkitnext_base INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(tetherkitnext_base INTERFACE tetherkitnext_warnings tetherkitnext_sanitizers
                                               tetherkitnext_optimizations)

# ------------------------------------------------------------------------------
# Version: generated from a template, so a version change does not trigger a full rebuild
# ------------------------------------------------------------------------------
# Build identity. The marketing version alone cannot tell two builds apart
# (every 0.2.0 prerelease reports "0.2.0"), so the app could not notice that
# the background daemon was still running an older 0.2.0 binary. The build ID
# makes each commit distinguishable; the app compares it with the daemon's.
# Resolved at configure time (release builds always configure fresh);
# override with -DTETHERKITNEXT_BUILD_ID=... when building outside git.
set(TETHERKITNEXT_BUILD_ID "" CACHE STRING "Build identifier embedded in the version info")
set(_tk_build_id "${TETHERKITNEXT_BUILD_ID}")
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
set(TETHERKITNEXT_BUILD_ID_RESOLVED "${_tk_build_id}")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.cc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.cc"
               @ONLY)

add_library(tk_version STATIC "${CMAKE_CURRENT_BINARY_DIR}/version.cc")
target_link_libraries(tk_version PUBLIC tetherkitnext_base)

add_subdirectory(common)
add_subdirectory(rndis)
add_subdirectory(net)
add_subdirectory(usb)
add_subdirectory(core)
add_subdirectory(capi)
add_subdirectory(app)
