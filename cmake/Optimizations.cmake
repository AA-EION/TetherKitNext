# Optimizations.cmake —— 数据路径优化选项
#
# 提供 INTERFACE 目标 tetherkitnext_optimizations。
#
# 设计取舍：
#  * 默认不加 -march/-mcpu：Apple Silicon 的 baseline（armv8.4-a for M1）已经包含
#    我们需要的一切（LSE 原子指令、NEON），额外的 -mcpu=native 收益很小却让产物
#    绑定到具体芯片代次。需要时用 -DTETHERKITNEXT_NATIVE_ARCH=ON 打开。
#  * 不用 -O3：本项目热路径以 memcpy 与系统调用为主，-O3 的激进循环展开与向量化
#    对此几乎无收益，反而增大代码体积、恶化 I-cache 命中。-O2（RelWithDebInfo 默认）
#    是更优选择。
#  * -fno-plt / -fvisibility=hidden 减少间接跳转与符号表体积。

add_library(tetherkitnext_optimizations INTERFACE)

target_compile_options(
  tetherkitnext_optimizations
  INTERFACE
    # 内部符号默认隐藏：本项目不导出稳定 ABI，可让优化器更自由地内联。
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    # 严格别名对我们手写的线格式解析是危险的（大量 reinterpret_cast），关掉。
    # 换来的代价很小：热路径的实际瓶颈是系统调用而非别名分析。
    -fno-strict-aliasing)

if(TETHERKITNEXT_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-mcpu=native TK_HAS_MCPU_NATIVE)
  if(TK_HAS_MCPU_NATIVE)
    target_compile_options(tetherkitnext_optimizations INTERFACE -mcpu=native)
  else()
    message(WARNING "编译器不支持 -mcpu=native，已跳过本机微架构优化。")
  endif()
endif()

if(TETHERKITNEXT_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT TK_IPO_OK OUTPUT TK_IPO_MSG)
  if(TK_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON PARENT_SCOPE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON PARENT_SCOPE)
  else()
    message(WARNING "不支持 LTO，已跳过：${TK_IPO_MSG}")
  endif()
endif()
