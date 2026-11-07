// Unit tests of byte_order.h.
//
// Key verification: unaligned access must be correct (the offset of an RNDIS message in the USB buffer is not guaranteed to be aligned).
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

TEST_CASE("little-endian loads follow the spec's byte order") {
  // The little-endian representation of 0x44332211 is 11 22 33 44.
  const std::array<std::byte, 8> bytes{std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                                       std::byte{0x44}, std::byte{0x55}, std::byte{0x66},
                                       std::byte{0x77}, std::byte{0x88}};
  CHECK(LoadLe16(bytes.data()) == 0x2211U);
  CHECK(LoadLe32(bytes.data()) == 0x44332211U);
  CHECK(LoadLe64(bytes.data()) == 0x8877665544332211ULL);
}

TEST_CASE("big-endian loads use network byte order") {
  const std::array<std::byte, 2> bytes{std::byte{0x08}, std::byte{0x00}};
  // 0x0800 = ETHERTYPE_IP
  CHECK(LoadBe16(bytes.data()) == 0x0800U);
}

TEST_CASE("a stored value reads back unchanged") {
  std::array<std::byte, 16> buffer{};
  StoreLe32(buffer.data(), 0xDEADBEEFU);
  CHECK(LoadLe32(buffer.data()) == 0xDEADBEEFU);

  StoreLe64(buffer.data() + 8, 0x0123456789ABCDEFULL);
  CHECK(LoadLe64(buffer.data() + 8) == 0x0123456789ABCDEFULL);

  StoreBe16(buffer.data() + 4, 0x86DDU);  // ETHERTYPE_IPV6
  CHECK(LoadBe16(buffer.data() + 4) == 0x86DDU);
  // The byte order of a big-endian write must be most-significant first.
  CHECK(buffer[4] == std::byte{0x86});
  CHECK(buffer[5] == std::byte{0xDD});
}

TEST_CASE("loads and stores at unaligned offsets are correct") {
  // This is the scenario most prone to errors in this project: the start offset of the Ethernet frame after REMOTE_NDIS_PACKET_MSG
  // is determined by the device's DataOffset and may be arbitrary.
  std::array<std::byte, 32> buffer{};
  for (std::size_t offset = 0; offset < 8; ++offset) {
    StoreLe32(buffer.data() + offset, 0xCAFEBABEU);
    CHECK(LoadLe32(buffer.data() + offset) == 0xCAFEBABEU);

    StoreLe64(buffer.data() + offset + 8, 0xFEEDFACEDEADBEEFULL);
    CHECK(LoadLe64(buffer.data() + offset + 8) == 0xFEEDFACEDEADBEEFULL);
  }
}

TEST_CASE("single-byte loads and stores do no conversion") {
  std::array<std::byte, 1> buffer{};
  tetherkitnext::StoreLe<std::uint8_t>(buffer.data(), 0xA5U);
  CHECK(tetherkitnext::LoadLe<std::uint8_t>(buffer.data()) == 0xA5U);
}

TEST_CASE("AlignUp rounds up to a power of two") {
  CHECK(AlignUp<std::uint32_t>(0, 4) == 0);
  CHECK(AlignUp<std::uint32_t>(1, 4) == 4);
  CHECK(AlignUp<std::uint32_t>(4, 4) == 4);
  CHECK(AlignUp<std::uint32_t>(5, 4) == 8);
  // RNDIS multi-packet aggregation requires each message to be aligned to 4 bytes.
  CHECK(AlignUp<std::uint32_t>(1514 + 44, 4) == 1560);
  CHECK(AlignUp<std::uint32_t>(63, 64) == 64);
}

TEST_CASE("IsPowerOfTwo recognizes powers of two") {
  CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(0));
  CHECK(IsPowerOfTwo<std::uint32_t>(1));
  CHECK(IsPowerOfTwo<std::uint32_t>(2));
  CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(3));
  CHECK(IsPowerOfTwo<std::uint32_t>(1024));
  CHECK_FALSE(IsPowerOfTwo<std::uint32_t>(1023));
}

}  // TEST_SUITE("common.byte_order")
