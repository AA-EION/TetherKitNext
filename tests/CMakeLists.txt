# tests -- unit tests
#
# All tests are linked into a single executable, tetherkitnext_tests, because:
#  * doctest registration is global, so a single binary has the lowest startup overhead;
#  * running by module is possible just by filtering with doctest's --test-suite;
#  * on the CTest side, multiple cases are registered per test-suite, so failures are still located precisely.

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

# tk_rndis / tk_net both link tk_common PUBLICly, so there is no need to list it again (otherwise the linker reports
# "ignoring duplicate libraries").
# tk_core links tk_common/tk_rndis/tk_net/tk_usb PUBLICly, so listing it alone is enough.
# tk_capi links tk_version and tk_core PUBLICly, so neither of those needs to be listed again.
target_link_libraries(tetherkitnext_tests PRIVATE tetherkitnext_base doctest tk_capi)
# src/capi directory: capi_support.h is an internal implementation header and is not installed externally, but tests must be able to cover it directly.
target_include_directories(tetherkitnext_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                   "${PROJECT_SOURCE_DIR}/src/capi")

# ------------------------------------------------------------------------------
# CTest registration
#
# WARNING: We once hit a pitfall here where "the whole test suite silently stopped working yet everything was green"; pay attention to two points:
#
#  1. **The filter argument must not be quoted.** When written as `--test-suite="${_suite}"`, CMake passes
#     the quotes through as part of the argument (measured: ctest -V showed it passing
#     `"--test-suite="rndis.state_machine""`), and doctest matches no test case at all --
#     so "0 test cases ran", and doctest returns exit code 0 for zero matches, and ctest reports
#     "Passed". The real failure is completely swallowed.
#
#  2. So add one more safeguard: set **FAIL_REGULAR_EXPRESSION** on every case, failing as soon as
#     "assertions: 0" appears in the output. Whoever mistypes a suite name in the future will turn it red immediately.
#     Here you **must** use FAIL_ rather than PASS_REGULAR_EXPRESSION -- the latter
#     **replaces** the exit-code judgment (documented CMake behavior); once set, real assertion failures are
#     ignored instead, which digs the just-fixed pit right back. FAIL_ is layered on top of the exit code.
#
# In addition, a separate `all` case runs the whole binary as the **authoritative result** -- even if some suite
# was forgotten in the list below, it will still expose the failure.
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
  # Note: do **not** quote the argument after --test-suite=; see the explanation above.
  add_test(NAME "${_suite}" COMMAND tetherkitnext_tests --test-suite=${_suite})
  set_tests_properties("${_suite}" PROPERTIES FAIL_REGULAR_EXPRESSION "assertions: +0 \\|")
endforeach()
