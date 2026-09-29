// Cache-related compile-time constants.
#pragma once

#include <cstddef>

namespace tetherkitnext {

/// Cache line size. Hard-coded to 128.
///
/// Why 128: `sysctl hw.cachelinesize` on Apple Silicon reports **128**.
/// With the customary 64-byte alignment, the producer and consumer indices could still land on the same 128-byte
/// cache line, so false sharing would remain -- measured cross-core bouncing raises the per-entry cost of a lock-free queue
/// from single-digit nanoseconds to 60+ nanoseconds.
///
/// Why **not** `std::hardware_destructive_interference_size`:
/// It does exist on Apple libc++ (in `<new>`, `__cpp_lib_hardware_interference_size`
/// = 201703), but it reports **256** (constructive reports 64), which does not match the real 128.
/// Using it would double all the padding, wasting twice the cache and memory for nothing, and would also introduce cross-TU ABI warnings.
/// The measured hardware value is the correct one, so it is hard-coded.
inline constexpr std::size_t kCacheLineSize = 128;

/// Alignment specifier that gives a member an entire cache line to itself.
#define TETHERKITNEXT_CACHE_ALIGNED alignas(::tetherkitnext::kCacheLineSize)

}  // namespace tetherkitnext
