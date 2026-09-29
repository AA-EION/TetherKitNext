// doctest's main() implementation occupies a translation unit of its own.
//
// This way modifying any test file does not require recompiling the implementation part of doctest's 7000-line header,
// noticeably shortening incremental build time.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
