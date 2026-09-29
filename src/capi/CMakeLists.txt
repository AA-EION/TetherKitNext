# tk_capi -- C ABI layer
#
# The **only** extern "C" boundary in the whole project; its sole reason to exist is to serve the Swift GUI.
# It sits at the very top of the dependency chain (the same layer as app) and depends on core and all modules below it.
#
# It is built as an OBJECT library rather than STATIC so that the shared library libtetherkitnext gets **all** the object
# files. If built as a static library, the linker would only pick "referenced" members, and the C ABI's exported functions
# are referenced by nobody inside the library -- the result would be an empty dylib with no error at all.
# (The other route is -force_load, but an OBJECT library is the more direct expression in CMake.)

add_library(
  tk_capi OBJECT
  capi_support.cc
  core_foundation_support.cc
  environment.cc
  log_ring.cc
  managed_network_service.cc
  net_config.cc
  orphan_cleanup.cc
  process_runner.cc
  session.cc)

target_link_libraries(tk_capi PUBLIC tetherkitnext_base tk_version tk_core)

# Reading back the NIC IP configuration requires configd's dynamic store (the gateway and DNS actually in effect), which can only go through
# SystemConfiguration. This is a dependency unique to tk_capi -- lower-level modules never touch configd.
target_link_libraries(tk_capi PUBLIC "-framework CoreFoundation"
                                     "-framework SystemConfiguration")

# ------------------------------------------------------------------------------
# libtetherkitnext -- the shared library distributed externally
#
# Swift can only link shared libraries or system libraries, not a pile of CMake static targets, so this layer is required.
# ------------------------------------------------------------------------------
if(TETHERKITNEXT_BUILD_SHARED_LIB)
  add_library(tetherkitnext_shared SHARED)
  target_link_libraries(tetherkitnext_shared PRIVATE tk_capi)

  set_target_properties(
    tetherkitnext_shared
    PROPERTIES OUTPUT_NAME tetherkitnext
               VERSION "${PROJECT_VERSION}"
               SOVERSION "${PROJECT_VERSION_MAJOR}"
               # Let dependents load via @rpath instead of burning in an absolute path -- so that when the .app embeds
               # this dylib there is no need for a post-hoc install_name_tool (which would also require re-signing;
               # see the pitfall in section 7 of docs/GUI-SPIKE.md).
               INSTALL_NAME_DIR "@rpath")

  # Only export the C ABI; all other symbols (including C++ mangled names) are hidden. See the allowlist file for the reasons.
  target_link_options(tetherkitnext_shared PRIVATE
                      "-Wl,-exported_symbols_list,${PROJECT_SOURCE_DIR}/cmake/tetherkitnext_exports.txt")

  install(TARGETS tetherkitnext_shared LIBRARY DESTINATION lib)
endif()

# The C ABI header is installed for external use: after the Homebrew formula is installed, other projects can also use it for bindings.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/tetherkitnext/capi"
        DESTINATION include/tetherkitnext
        FILES_MATCHING
        PATTERN "*.h")
