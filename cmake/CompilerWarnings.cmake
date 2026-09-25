# CompilerWarnings.cmake —— 集中管理警告选项
#
# 提供 INTERFACE 目标 tetherkitnext_warnings，所有本项目自己的目标都 link 它；
# 第三方代码（third_party/）不 link，避免噪声。

add_library(tetherkitnext_warnings INTERFACE)

set(_tk_warnings
    -Wall
    -Wextra
    -Wpedantic
    # 隐式转换是协议解析代码最容易出错的地方，必须开。
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    # 数据路径上的隐式堆分配是性能杀手，让编译器帮忙盯着 VLA。
    -Wvla
    # 结构体填充会影响我们手写的线格式结构体，需要 static_assert 兜底，
    # 但 -Wpadded 噪声过大，因此不开，改用 static_assert(sizeof(...)) 校验。
)

target_compile_options(tetherkitnext_warnings INTERFACE ${_tk_warnings})

if(TETHERKITNEXT_WARNINGS_AS_ERRORS)
  target_compile_options(tetherkitnext_warnings INTERFACE -Werror)
endif()
