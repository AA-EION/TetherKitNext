# FindLibUSB.cmake -- locate libusb-1.0
#
# Prefer pkg-config (the libusb installed by Homebrew ships a .pc file, which correctly reports
# the unconventional header directory `include/libusb-1.0`); on failure, fall back to a manual search of common prefixes.
#
# Outputs:
#   LibUSB_FOUND        -- whether it was found
#   LibUSB_VERSION      -- version string
#   LibUSB_INCLUDE_DIR  -- directory containing libusb.h
#   LibUSB_LIBRARY      -- path to the library file
#   LibUSB::LibUSB      -- IMPORTED target that can be linked directly

# LibUSB_ROOT (see scripts/build-libusb.sh) pins a specific build — the
# universal, @rpath-named libusb that release builds embed in TetherKitNext.app.
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

# Version: pkg-config is the most reliable; otherwise infer a lower bound from LIBUSB_API_VERSION in the header.
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
  # libusb's darwin backend needs IOKit and CoreFoundation, plus Security (IOUSBHost permission checks).
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
