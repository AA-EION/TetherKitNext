# tk_rndis -- RNDIS protocol layer
#
# A pure-logic module: it only does wire-format encoding/decoding and state-machine simulation, **and touches no I/O**.
# This constraint is deliberate -- it lets the entire RNDIS implementation be fully covered by unit tests on
# machines without a real device (this project's development machines have neither USB devices nor root privileges).

add_library(
  tk_rndis STATIC
  protocol.cc
  messages.cc
  state_machine.cc)

target_link_libraries(tk_rndis PUBLIC tetherkitnext_base tk_common)
