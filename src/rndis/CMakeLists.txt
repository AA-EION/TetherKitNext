# tk_rndis —— RNDIS 协议层
#
# 纯逻辑模块：只做线格式编解码与状态机推演，**不碰任何 I/O**。
# 这个约束是刻意的 —— 它让整个 RNDIS 实现可以在没有真实设备的机器上被完整
# 单元测试覆盖（本项目的开发机既没有 USB 设备也没有 root 权限）。

add_library(
  tk_rndis STATIC
  protocol.cc
  messages.cc
  state_machine.cc)

target_link_libraries(tk_rndis PUBLIC tetherkitnext_base tk_common)
