// byte_order.h 的单元测试。
//
// 重点验证：未对齐访问必须正确（RNDIS 消息在 USB 缓冲里的偏移不保证对齐）。
#include <array>
#include <cstddef>
#include <cstdint>

#include <doctest.h>

#include "tetherkitnext/common/byte_order.h"

using tetherkitnext::AlignUp;
using tetherkitnext::IsPowerOfTwo;
using tetherkitnext::LoadBe16;
using tetherkitnext::LoadLe16;
using tetherkitnext::LoadLe32;
using tetherkitnext::LoadLe64;
using tetherkitnext::StoreBe16;
using tetherkitnext::StoreLe32;
using tetherkitnext::StoreLe64;

TEST_SUITE("common.byte_order") {

TEST_CASE("小端读取按规范解释字节序") {
  // 0x44332211 的小端表示是 11 22 33 44。
  const std::array<std::byte, 8> bytes{std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                                       std::byte{0x44}, std::byte{0x55}, std::byte{0x66},
                                       std::byte{0x77}, std::byte{0x88}};
  CHECK(LoadLe16(bytes.data()) == 0x2211U);
  CHECK(LoadLe32(bytes.data()) == 0x44332211U);
  CHECK(LoadLe64(bytes.data()) == 0x8877665544332211ULL);
}

TEST_CASE("大端读取按网络字节序解释") {
  const std::array<std::byte, 2> bytes{std::byte{0x08}, std::byte{0x00}};
  // 0x0800 = ETHERTYPE_IP
  CHECK(LoadBe16(bytes.data()) == 0x0800U);
}

TEST_CASE("写入后读回保持一致") {
  std::array<std::byte, 16> buffer{};
  StoreLe32(buffer.data(), 0xDEADBEEFU);
  CHECK(LoadLe32(buffer.data()) == 0xDEADBEEFU);

  StoreLe64(buffer.data() + 8, 0x0123456789ABCDEFULL);
  CHECK(LoadLe64(buffer.data() + 8) == 0x0123456789ABCDEFULL);

  StoreBe16(buffer.data() + 4, 0x86DDU);  // ETHERTYPE_IPV6
  CHECK(LoadBe16(buffer.data() + 4) == 0x86DDU);
  // 大端写入的字节顺序必须是高位在前。
  CHECK(buffer[4] == std::byte{0x86});
  CHECK(buffer[5] == std::byte{0xDD});
}

TEST_CASE("未对齐偏移上的读写同样正确") {
  // 这是本项目最容易出错的场景：REMOTE_NDIS_PACKET_MSG 之后的以太帧
  // 起始偏移由设备的 DataOffset 决定，可能是任意值。
  std::array<std::byte, 32> buffer{};
  for (std::size_t offset = 0; offset < 8; ++offset) {
    StoreLe32(buffer.data() + offset, 0xCAFEBABEU);
    CHECK(LoadLe32(buffer.data() + offset) == 0xCAFEBABEU);

    StoreLe64(buffer.data() + offset + 8, 0xFEEDFACEDEADBEEFULL);
    CHECK(LoadLe64(buffer.data() + offset + 8) == 0xFEEDFACEDEADBEEFULL);
  }
}

TEST_CASE("单字节读写不做任何转换") {
  std::array<std::byte, 1> buffer{};
  tetherkitnext::StoreLe<std::uint8_t>(buffer.data(), 0xA5U);
  CHECK(tetherkitnext::LoadLe<std::uint8_t>(buffer.data()) == 0xA5U);
}

TEST_CASE("AlignUp 向上对齐到 2 的幂") {
  CHECK(AlignUp<std::uint32_t>(0, 4) == 0);
  CHECK(AlignUp<std::uint32_t>(1, 4) == 4);
  CHECK(AlignUp<std::uint32_t>(4, 4) == 4);
  CHECK(AlignUp<std::uint32_t>(5, 4) == 8);
  // RNDIS 多包聚合要求每条消息按 4 字节对齐。
  CHECK(AlignUp<std::uint32_t>(1514 + 44, 4) == 1560);
  CHECK(AlignUp<std::uint32_t>(63, 64) == 64);
}

TEST_CASE("IsPowerOfTwo 正确识别 2 的幂") {
  CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(0));
  CHECK(IsPowerOfTwo<std::uint32_t>(1));
  CHECK(IsPowerOfTwo<std::uint32_t>(2));
  CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(3));
  CHECK(IsPowerOfTwo<std::uint32_t>(1024));
  CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(1023));
}

}  // TEST_SUITE("common.byte_order")
