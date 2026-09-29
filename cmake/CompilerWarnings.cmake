# CompilerWarnings.cmake -- centralized management of warning options
#
# Provides the INTERFACE target tetherkitnext_warnings, which all of this project's own targets link;
# third-party code (third_party/) does not link it, to avoid noise.

add_library(tetherkitnext_warnings INTERFACE)

set(_tk_warnings
    -Wall
    -Wextra
    -Wpedantic
    # Implicit conversions are where protocol-parsing code most easily goes wrong; must be enabled.
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    # Implicit heap allocation on the data path is a performance killer; let the compiler watch for VLAs.
    -Wvla
    # Struct padding affects our hand-written wire-format structs and needs a static_assert backstop,
    # but -Wpadded is too noisy, so it is not enabled; static_assert(sizeof(...)) checks are used instead.
)

target_compile_options(tetherkitnext_warnings INTERFACE ${_tk_warnings})

if(TETHERKITNEXT_WARNINGS_AS_ERRORS)
  target_compile_options(tetherkitnext_warnings INTERFACE -Werror)
endif()
