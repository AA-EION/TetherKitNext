// Wire-format byte order reads and writes.
//
// The RNDIS wire format is **fixed little-endian** (the protocol originates from Windows NDIS), while fields such as
// EtherType in the Ethernet frame header are big-endian. Both are needed, so both Le/Be families of functions are provided here.
//
// Implementation notes:
//   * Everything goes through std::memcpy rather than pointer casts, avoiding the undefined behavior of unaligned accesses --
//     the start offset of an RNDIS message in the USB transfer buffer is only guaranteed to be 4-byte aligned, and
//     the start offset of the Ethernet frame right after REMOTE_NDIS_PACKET_MSG can be arbitrary.
//     clang optimizes "memcpy into a local variable + byteswap" into a single ldr/rev instruction,
//     so this is a zero-cost abstraction.
//   * std::endian is used to branch at compile time, so it is also correct on big-endian hosts (although macOS has none).
#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tetherkitnext {

/// Unsigned integer types that appear in the wire format.
template <typename T>
concept WireUnsigned = std::unsigned_integral<T> && !std::same_as<T, bool> &&
                       (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

/// Reads an integer from little-endian byte order.
template <WireUnsigned T>
[[nodiscard]] inline T LoadLe(const std::byte* src) noexcept {
  T value{};
  std::memcpy(&value, src, sizeof(T));
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  return value;
}

/// Writes an integer in little-endian byte order.
template <WireUnsigned T>
inline void StoreLe(std::byte* dst, T value) noexcept {
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  std::memcpy(dst, &value, sizeof(T));
}

/// Reads an integer from big-endian (network byte order).
template <WireUnsigned T>
[[nodiscard]] inline T LoadBe(const std::byte* src) noexcept {
  T value{};
  std::memcpy(&value, src, sizeof(T));
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
    value = std::byteswap(value);
  }
  return value;
}

/// Writes an integer in big-endian (network byte order).
template <WireUnsigned T>
inline void StoreBe(std::byte* dst, T value) noexcept {
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
    value = std::byteswap(value);
  }
  std::memcpy(dst, &value, sizeof(T));
}

// Convenience aliases for common widths, so protocol-parsing code reads closer to the field types in the spec documents.
[[nodiscard]] inline std::uint16_t LoadLe16(const std::byte* p) noexcept {
  return LoadLe<std::uint16_t>(p);
}

[[nodiscard]] inline std::uint32_t LoadLe32(const std::byte* p) noexcept {
  return LoadLe<std::uint32_t>(p);
}

[[nodiscard]] inline std::uint64_t LoadLe64(const std::byte* p) noexcept {
  return LoadLe<std::uint64_t>(p);
}

inline void StoreLe16(std::byte* p, std::uint16_t v) noexcept {
  StoreLe<std::uint16_t>(p, v);
}

inline void StoreLe32(std::byte* p, std::uint32_t v) noexcept {
  StoreLe<std::uint32_t>(p, v);
}

inline void StoreLe64(std::byte* p, std::uint64_t v) noexcept {
  StoreLe<std::uint64_t>(p, v);
}

[[nodiscard]] inline std::uint16_t LoadBe16(const std::byte* p) noexcept {
  return LoadBe<std::uint16_t>(p);
}

inline void StoreBe16(std::byte* p, std::uint16_t v) noexcept {
  StoreBe<std::uint16_t>(p, v);
}

/// Rounds up to a multiple of `alignment`. `alignment` must be a power of two.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T AlignUp(T value, T alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

/// Checks whether the value is a power of two (0 is not).
template <std::unsigned_integral T>
[[nodiscard]] constexpr bool IsPowerOfTwo(T value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace tetherkitnext
