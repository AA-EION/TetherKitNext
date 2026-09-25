#include "tetherkitnext/common/error.h"

#include <libusb.h>

#include <array>
#include <cstring>
#include <format>

namespace tetherkitnext {
namespace {

/// 线程安全的 strerror。
///
/// 不能用 std::strerror：它返回指向静态缓冲的指针，多线程同时调用会互相覆盖
/// （clang-tidy 的 concurrency-mt-unsafe 就在提示这点）。
std::string SafeStrerror(int err) {
  std::array<char, 128> buffer{};
  // Darwin 提供 XSI 版 strerror_r，返回 int：0 成功，ERANGE 表示缓冲太小。
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
      // RNDIS_STATUS_* 到名字的映射只存在于 tk_rndis（rndis/protocol.cc），
      // tk_common 不允许依赖上层模块，因此这里只输出原始数值 ——
      // 状态码的符号名由 rndis 层在构造 Error 时拼进 context 串。
      return std::format("{} [RNDIS_STATUS: {:#010x}]", context_,
                         static_cast<std::uint32_t>(code_));
    }
  }
  return context_;
}

}  // namespace tetherkitnext
