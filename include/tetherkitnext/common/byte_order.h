// 线格式字节序读写。
//
// RNDIS 的线格式**固定小端**（协议源自 Windows NDIS），而以太帧头里的
// EtherType 等字段是大端。两种都要用，因此这里同时提供 Le/Be 两族函数。
//
// 实现要点：
//   * 全部走 std::memcpy 而非指针强转，避免未对齐访问的未定义行为 ——
//     RNDIS 消息在 USB 传输缓冲里的起始偏移只保证 4 字节对齐，而
//     REMOTE_NDIS_PACKET_MSG 之后紧跟的以太帧起始偏移可能是任意值。
//     clang 会把「memcpy 到局部变量 + byteswap」优化成单条 ldr/rev 指令，
//     所以这是零成本抽象。
//   * 用 std::endian 在编译期分支，big-endian 主机上也正确（虽然 macOS 不存在）。
#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tetherkitnext {

/// 线格式里出现的无符号整数类型。
template <typename T>
concept WireUnsigned = std::unsigned_integral<T> && !std::same_as<T, bool> &&
                       (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

/// 从小端字节序读出一个整数。
template <WireUnsigned T>
[[nodiscard]] inline T LoadLe(const std::byte* src) noexcept {
  T value{};
  std::memcpy(&value, src, sizeof(T));
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  return value;
}

/// 把一个整数按小端字节序写入。
template <WireUnsigned T>
inline void StoreLe(std::byte* dst, T value) noexcept {
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  std::memcpy(dst, &value, sizeof(T));
}

/// 从大端（网络字节序）读出一个整数。
template <WireUnsigned T>
[[nodiscard]] inline T LoadBe(const std::byte* src) noexcept {
  T value{};
  std::memcpy(&value, src, sizeof(T));
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
    value = std::byteswap(value);
  }
  return value;
}

/// 把一个整数按大端（网络字节序）写入。
template <WireUnsigned T>
inline void StoreBe(std::byte* dst, T value) noexcept {
  if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
    value = std::byteswap(value);
  }
  std::memcpy(dst, &value, sizeof(T));
}

// 常用宽度的便捷别名，让协议解析代码读起来更贴近规范文档里的字段类型。
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

/// 向上对齐到 `alignment` 的整数倍。`alignment` 必须是 2 的幂。
template <std::unsigned_integral T>
[[nodiscard]] constexpr T AlignUp(T value, T alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

/// 判断是否为 2 的幂（0 不是）。
template <std::unsigned_integral T>
[[nodiscard]] constexpr bool IsPowerOfTwo(T value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace tetherkitnext
