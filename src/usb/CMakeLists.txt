# tk_usb —— USB 传输层
#
# 封装 libusb：设备发现与识别、接口声明、控制通道、中断通知、异步 bulk 传输池。
# 依赖 tk_rndis 只为拿协议常量（接口签名、控制请求码、通知格式），不含状态机。

add_library(
  tk_usb STATIC
  context.cc
  device.cc
  data_channel.cc)

target_link_libraries(tk_usb PUBLIC tetherkitnext_base tk_common tk_rndis)
target_link_libraries(tk_usb PUBLIC LibUSB::LibUSB)
