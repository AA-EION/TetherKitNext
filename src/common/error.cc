#include "tetherkitnext/common/error.h"

#include <libusb.h>

#include <array>
#include <cstring>
#include <format>

namespace tetherkitnext {
namespace {

/// Thread-safe strerror.
///
/// std::strerror cannot be used: it returns a pointer to a static buffer, and concurrent calls from multiple threads overwrite each other
/// (which is what clang-tidy's concurrency-mt-unsafe is hinting at).
std::string SafeStrerror(int err) {
  std::array<char, 128> buffer{};
  // Darwin provides the XSI version of strerror_r, returning int: 0 on success, ERANGE means the buffer is too small.
  if (::strerror_r(err, buffer.data(), buffer.size()) != 0) {
    return std::format("errno {}", err);
  }
  return buffer.data();
}

}  // namespace

std::string Error::ToString() const {
  switch (domain_) {
    case ErrorDomain::kGeneric:
      return context_;

    case ErrorDomain::kErrno: {
      const int err = static_cast<int>(code_);
      return std::format("{} [errno: {}({})]", context_, SafeStrerror(err), err);
    }

    case ErrorDomain::kLibUsb: {
      const int rc = static_cast<int>(code_);
      return std::format("{} [libusb: {}({})]", context_, libusb_error_name(rc), rc);
    }

    case ErrorDomain::kRndis: {
      // The mapping from RNDIS_STATUS_* to names exists only in tk_rndis (rndis/protocol.cc);
      // tk_common is not allowed to depend on upper-layer modules, so only the raw numeric value is output here --
      // the symbolic name of the status code is spliced into the context string by the rndis layer when it constructs the Error.
      return std::format("{} [RNDIS_STATUS: {:#010x}]", context_,
                         static_cast<std::uint32_t>(code_));
    }
  }
  return context_;
}

}  // namespace tetherkitnext
