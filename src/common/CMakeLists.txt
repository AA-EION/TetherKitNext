# tk_common -- infrastructure layer
#
# Depends on no upper-layer module, nor on libusb / network system calls. The only external dependency is
# libusb's header files (error.cc needs libusb_error_name to translate error codes), so it is linked
# PRIVATE and libusb is not propagated to users of tk_common.

add_library(
  tk_common STATIC
  error.cc
  i18n.cc
  logging.cc
  scheduling.cc)

target_link_libraries(tk_common PUBLIC tetherkitnext_base)
target_link_libraries(tk_common PRIVATE LibUSB::LibUSB)
