# tk_common —— 基础设施层
#
# 不依赖任何上层模块，也不依赖 libusb / 网络系统调用。唯一的外部依赖是
# libusb 的头文件（error.cc 需要 libusb_error_name 来翻译错误码），因此
# 以 PRIVATE 方式链接，不把 libusb 传播给 tk_common 的使用者。

add_library(
  tk_common STATIC
  error.cc
  i18n.cc
  logging.cc
  scheduling.cc)

target_link_libraries(tk_common PUBLIC tetherkitnext_base)
target_link_libraries(tk_common PRIVATE LibUSB::LibUSB)
