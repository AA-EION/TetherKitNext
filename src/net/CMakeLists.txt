# tk_net -- virtual NIC and link layer
#
# Depends only on macOS system calls (ioctl / BPF), not on libusb nor on the RNDIS protocol layer.
# LinkBackend is this module's public abstraction; BpfLink is the production implementation and LoopbackLink is a pure in-memory implementation
# -- the latter lets the bridge layer's end-to-end tests and throughput benchmarks run on machines without root or real interfaces.

add_library(
  tk_net STATIC
  feth_device.cc
  bpf_link.cc
  loopback_link.cc)

target_link_libraries(tk_net PUBLIC tetherkitnext_base tk_common)
