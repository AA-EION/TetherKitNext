// 版本信息的冒烟测试：确认 CMake 的版本注入链路是通的。
#include <doctest.h>

#include "tetherkitnext/version.h"

TEST_SUITE("version") {

TEST_CASE("版本号非全零") {
  const auto v = tetherkitnext::GetVersion();
  CHECK((v.major != 0 || v.minor != 0 || v.patch != 0));
}

TEST_CASE("版本串包含项目名") {
  const std::string_view s = tetherkitnext::GetVersionString();
  CHECK(s.find("TetherKitNext") != std::string_view::npos);
}

}  // TEST_SUITE("version")
