// Smoke test of version information: confirms the CMake version injection chain works.
#include <doctest.h>

#include "tetherkitnext/version.h"

TEST_SUITE("version") {

TEST_CASE("the version is not all zeros") {
  const auto v = tetherkitnext::GetVersion();
  CHECK((v.major != 0 || v.minor != 0 || v.patch != 0));
}

TEST_CASE("the version string contains the project name") {
  const std::string_view s = tetherkitnext::GetVersionString();
  CHECK(s.find("TetherKitNext") != std::string_view::npos);
}

}  // TEST_SUITE("version")
