// 缓存相关的编译期常量。
#pragma once

#include <cstddef>

namespace tetherkitnext {

/// 缓存行大小。硬编码 128。
///
/// 为什么是 128：Apple Silicon 的 `sysctl hw.cachelinesize` 报 **128**。
/// 按习惯性的 64 对齐的话，生产者与消费者的索引仍可能落在同一条 128 字节
/// 缓存行上，false sharing 依然存在 —— 实测跨核弹跳会让无锁队列的每条目成本
/// 从个位数纳秒涨到 60+ 纳秒。
///
/// 为什么**不用** `std::hardware_destructive_interference_size`：
/// 它在 Apple libc++ 上确实存在（在 `<new>` 里，`__cpp_lib_hardware_interference_size`
/// = 201703），但报的是 **256**（constructive 报 64），与真实的 128 不符。
/// 用它会把所有填充翻倍，白白浪费一倍的缓存与内存，还会引入跨 TU 的 ABI 警告。
/// 实测硬件值才是对的，所以硬编码。
inline constexpr std::size_t kCacheLineSize = 128;

/// 让成员独占一条缓存行的对齐说明符。
#define TETHERKITNEXT_CACHE_ALIGNED alignas(::tetherkitnext::kCacheLineSize)

}  // namespace tetherkitnext
