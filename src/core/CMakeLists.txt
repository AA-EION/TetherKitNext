# tk_core —— 数据路径桥接与运行时编排
#
# 把 tk_usb 的 RNDIS 数据通道与 tk_net 的链路后端对接起来，并管理数据路径线程。
# 它是唯一同时依赖 usb / net / rndis 三个模块的层。

add_library(
  tk_core STATIC
  bridge.cc
  runtime.cc)

target_link_libraries(tk_core PUBLIC tetherkitnext_base tk_common tk_rndis tk_net tk_usb)
