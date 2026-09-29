// Unified error representation and propagation utilities.
//
// Layering principle (consistent across the whole project):
//   * Initialization path / control path -- propagate errors with Result<T>; the error object carries the source domain,
//     the raw error code and a human-readable context string, making it possible to translate "libusb returned -3" into
//     "failed to claim the RNDIS data interface: LIBUSB_ERROR_ACCESS (root required)".
//   * Data hot path -- **never** use this header. The std::string inside Result<T> allocates heap memory,
//     which is unacceptable at 25k~80k pps. The hot path expresses failures with return counts + atomic statistics counters.
#pragma once

#include <cerrno>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tetherkitnext {

namespace detail {

/// The separator in "outer cause <separator> inner cause", which changes with the current language.
///
/// Forward-declared here (implemented in common/i18n.cc) rather than including i18n.h directly:
/// error.h is included by the whole project, and making it drag in <format> and the entire message table is not worthwhile.
[[nodiscard]] std::string_view ContextSeparator() noexcept;

}  // namespace detail

/// The source domain of an error code. It decides how the `code` field is translated into text.
enum class ErrorDomain : std::uint8_t {
  kGeneric,  ///< Pure logic error; `code` is meaningless.
  kErrno,    ///< POSIX errno (system calls such as open / ioctl / read / write).
  kLibUsb,   ///< libusb's enum libusb_error (negative values).
  kRndis,    ///< RNDIS_STATUS_* status codes (32-bit unsigned).
};

/// A complete description of a failure.
///
/// Deliberately a value type rather than an exception: the vast majority of this project's errors are "expected environment problems"
/// (no device plugged in, no root, a kernel driver holding the interface); callers always have to handle them, so they should not propagate via exceptions.
class Error {
 public:
  Error() = default;

  /// Pure logic error; description only.
  static Error Generic(std::string context) {
    return Error{ErrorDomain::kGeneric, 0, std::move(context)};
  }

  /// Wraps POSIX errno. When `err` is 0, the global errno is read automatically.
  static Error FromErrno(int err, std::string context) {
    return Error{ErrorDomain::kErrno, err != 0 ? err : errno, std::move(context)};
  }

  /// Wraps a libusb error code (libusb error codes are negative; kept as-is here).
  static Error FromLibUsb(int rc, std::string context) {
    return Error{ErrorDomain::kLibUsb, rc, std::move(context)};
  }

  /// Wraps RNDIS_STATUS_*.
  static Error FromRndisStatus(std::uint32_t status, std::string context) {
    return Error{ErrorDomain::kRndis, static_cast<std::int64_t>(status), std::move(context)};
  }

  [[nodiscard]] ErrorDomain Domain() const noexcept { return domain_; }

  [[nodiscard]] std::int64_t Code() const noexcept { return code_; }

  [[nodiscard]] std::string_view Context() const noexcept { return context_; }

  /// Appends a layer of context to an existing error, forming an "outer cause: inner cause" chain.
  /// Usage: `return std::unexpected(std::move(e).WithContext(Tr(Msg::kNetOpenBpfFailed)));`
  ///
  /// The separator changes with the language (a full-width colon in Chinese, ": " in English), so it cannot be hard-coded here.
  [[nodiscard]] Error WithContext(std::string_view outer) && {
    context_ = std::string{outer} + std::string{detail::ContextSeparator()} + context_;
    return std::move(*this);
  }

  /// Renders a readable string like `failed to claim data interface [libusb: LIBUSB_ERROR_ACCESS(-3)]`.
  [[nodiscard]] std::string ToString() const;

 private:
  Error(ErrorDomain domain, std::int64_t code, std::string context)
      : code_(code), context_(std::move(context)), domain_(domain) {}

  std::int64_t code_ = 0;
  std::string context_;
  ErrorDomain domain_ = ErrorDomain::kGeneric;
};

/// An operation that may fail and returns a value.
template <typename T>
using Result = std::expected<T, Error>;

/// An operation that may fail but returns no value.
using Status = std::expected<void, Error>;

/// A literal for a successful Status.
inline Status Ok() noexcept {
  return Status{};
}

namespace detail {

// Used to generate unique temporary variable names, avoiding variable shadowing during macro expansion.
#define TETHERKITNEXT_CONCAT_INNER(a, b) a##b
#define TETHERKITNEXT_CONCAT(a, b) TETHERKITNEXT_CONCAT_INNER(a, b)
#define TETHERKITNEXT_UNIQUE(base) TETHERKITNEXT_CONCAT(base, __LINE__)

}  // namespace detail

/// Immediately propagates the error upward if the expression fails.
///
/// Usage: `TETHERKITNEXT_RETURN_IF_ERROR(link.Configure(mtu));`
#define TETHERKITNEXT_RETURN_IF_ERROR(expr)                                      \
  do {                                                                       \
    auto TETHERKITNEXT_UNIQUE(tk_status_) = (expr);                              \
    if (!TETHERKITNEXT_UNIQUE(tk_status_)) {                                     \
      return std::unexpected(std::move(TETHERKITNEXT_UNIQUE(tk_status_)).error()); \
    }                                                                        \
  } while (false)

/// Extracts the success value into a new variable; on failure, propagates the error upward.
///
/// Usage: `TETHERKITNEXT_ASSIGN_OR_RETURN(auto fd, OpenBpfDevice());`
///
/// `decl` is a declaration rather than an expression and cannot be parenthesized, so the macro-parentheses check is exempted here.
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define TETHERKITNEXT_ASSIGN_OR_RETURN(decl, expr)                              \
  auto TETHERKITNEXT_UNIQUE(tk_result_) = (expr);                               \
  if (!TETHERKITNEXT_UNIQUE(tk_result_)) {                                      \
    return std::unexpected(std::move(TETHERKITNEXT_UNIQUE(tk_result_)).error()); \
  }                                                                         \
  decl = std::move(TETHERKITNEXT_UNIQUE(tk_result_)).value()

}  // namespace tetherkitnext
