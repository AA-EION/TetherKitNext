# Sanitizers.cmake -- sanitizer switches
#
# Provides the INTERFACE target tetherkitnext_sanitizers.
#
# Notes:
#  * ASan and TSan are mutually exclusive and cannot be enabled together.
#  * TSan is this project's most important sanitizer -- the memory-ordering correctness among the lock-free SPSC rings, the libusb event thread and
#    the BPF read/write threads can only be verified with it. It is recommended to run at least once:
#      cmake -B build-tsan -DTETHERKITNEXT_ENABLE_TSAN=ON && ctest --test-dir build-tsan
#  * Sanitizers significantly reduce throughput; benchmarks must be run on a Release build without sanitizers.

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
  # Would hand-written wire-format structs cause unaligned accesses? No -- we read everything byte by byte,
  # so the alignment check stays on, to catch real bugs.
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
