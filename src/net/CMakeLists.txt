# tk_net —— 虚拟网卡与链路层
#
# 只依赖 macOS 系统调用（ioctl / BPF），不依赖 libusb 也不依赖 RNDIS 协议层。
# LinkBackend 是本模块的对外抽象；BpfLink 是生产实现，LoopbackLink 是纯内存实现
# —— 后者让桥接层的端到端测试与吞吐基准能在没有 root、没有真实接口的机器上跑。

add_library(
  tk_net STATIC
  feth_device.cc
  bpf_link.cc
  loopback_link.cc)

target_link_libraries(tk_net PUBLIC tetherkitnext_base tk_common)
