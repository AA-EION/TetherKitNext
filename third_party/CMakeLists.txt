# third_party -- vendored third-party dependencies
#
# Deliberately vendoring rather than FetchContent: no network access at configure time, guaranteeing that
# `cmake -B build` succeeds repeatably at any time and in any network environment.

# doctest 2.4.12 -- single-header test framework.
# Chosen over GoogleTest/Catch2 because: a single header file, fast compile speed, sufficient assertion expressiveness,
# native SUBCASE support for parameterization, and it can be used safely for assertions in multithreaded tests.
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")
