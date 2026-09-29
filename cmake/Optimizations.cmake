# Optimizations.cmake -- data-path optimization options
#
# Provides the INTERFACE target tetherkitnext_optimizations.
#
# Design trade-offs:
#  * -march/-mcpu are not added by default: the Apple Silicon baseline (armv8.4-a for M1) already contains
#    everything we need (LSE atomics, NEON); an additional -mcpu=native gains little but ties the artifact
#    to a specific chip generation. Turn it on when needed with -DTETHERKITNEXT_NATIVE_ARCH=ON.
#  * -O3 is not used: this project's hot path is dominated by memcpy and system calls, for which -O3's aggressive loop unrolling and
#    vectorization bring almost no benefit while increasing code size and worsening I-cache hit rate. -O2 (the RelWithDebInfo default)
#    is the better choice.
#  * -fno-plt / -fvisibility=hidden reduce indirect jumps and symbol table size.

add_library(tetherkitnext_optimizations INTERFACE)

target_compile_options(
  tetherkitnext_optimizations
  INTERFACE
    # Internal symbols are hidden by default: this project does not export a stable ABI, which lets the optimizer inline more freely.
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    # Strict aliasing is dangerous for our hand-written wire-format parsing (lots of reinterpret_cast), so it is turned off.
    # The cost is small: the real bottleneck on the hot path is system calls, not alias analysis.
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
