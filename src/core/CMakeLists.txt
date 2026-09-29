# tk_core -- data-path bridge and runtime orchestration
#
# Connects tk_usb's RNDIS data channel to tk_net's link backend and manages the data-path threads.
# It is the only layer that depends on all three of the usb / net / rndis modules.

add_library(
  tk_core STATIC
  bridge.cc
  runtime.cc)

target_link_libraries(tk_core PUBLIC tetherkitnext_base tk_common tk_rndis tk_net tk_usb)
