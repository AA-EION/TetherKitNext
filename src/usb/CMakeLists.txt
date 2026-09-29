# tk_usb -- USB transport layer
#
# Wraps libusb: device discovery and identification, interface claiming, control channel, interrupt notifications, async bulk transfer pool.
# It depends on tk_rndis only to get protocol constants (interface signatures, control request codes, notification format), not the state machine.

add_library(
  tk_usb STATIC
  context.cc
  device.cc
  data_channel.cc)

target_link_libraries(tk_usb PUBLIC tetherkitnext_base tk_common tk_rndis)
target_link_libraries(tk_usb PUBLIC LibUSB::LibUSB)
