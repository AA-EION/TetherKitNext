// 统一的错误表示与传播工具。
//
// 分层原则（全项目一致）：
//   * 初始化路径 / 控制路径 —— 用 Result<T> 传播错误，错误对象携带来源域、
//     原始错误码与人类可读的上下文串，便于把「libusb 返回 -3」翻译成
//     「声明 RNDIS 数据接口失败：LIBUSB_ERROR_ACCESS（需要 root）」。
//   * 数据热路径 —— **绝不**使用本头文件。Result<T> 里的 std::string 会分配堆内存，
//     在 25k~80k pps 下不可接受。热路径用返回计数 + 原子统计计数器表达失败。
#pragma once

#include <cerrno>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tetherkitnext {

namespace detail {

/// 「外层原因 <分隔符> 内层原因」里的那个分隔符，随当前语言变化。
///
/// 在这里前置声明（实现在 common/i18n.cc）而不是直接 include i18n.h：
/// error.h 被全项目包含，让它拖上 <format> 与整张文案表不划算。
[[nodiscard]] std::string_view ContextSeparator() noexcept;

}  // namespace detail

/// 错误码的来源域。决定 `code` 字段该怎么翻译成文字。
enum class ErrorDomain : std::uint8_t {
  kGeneric,  ///< 纯逻辑错误，`code` 无意义。
  kErrno,    ///< POSIX errno（open / ioctl / read / write 等系统调用）。
  kLibUsb,   ///< libusb 的 enum libusb_error（负值）。
  kRndis,    ///< RNDIS_STATUS_* 状态码（32 位无符号）。
};

/// 一次失败的完整描述。
///
/// 刻意做成值类型而非异常：本项目的错误绝大多数是「预期内的环境问题」
/// （没插设备、没有 root、内核驱动占用接口），调用方总要处理，不该靠异常传播。
class Error {
 public:
  Error() = default;

  /// 纯逻辑错误，只有描述。
  static Error Generic(std::string context) {
    return Error{ErrorDomain::kGeneric, 0, std::move(context)};
  }

  /// 包装 POSIX errno。`err` 传 0 时自动读取全局 errno。
  static Error FromErrno(int err, std::string context) {
    return Error{ErrorDomain::kErrno, err != 0 ? err : errno, std::move(context)};
  }

  /// 包装 libusb 错误码（libusb 的错误码是负值，此处原样保留）。
  static Error FromLibUsb(int rc, std::string context) {
    return Error{ErrorDomain::kLibUsb, rc, std::move(context)};
  }

  /// 包装 RNDIS_STATUS_*。
  static Error FromRndisStatus(std::uint32_t status, std::string context) {
    return Error{ErrorDomain::kRndis, static_cast<std::int64_t>(status), std::move(context)};
  }

  [[nodiscard]] ErrorDomain Domain() const noexcept { return domain_; }

  [[nodiscard]] std::int64_t Code() const noexcept { return code_; }

  [[nodiscard]] std::string_view Context() const noexcept { return context_; }

  /// 在已有错误上追加一层上下文，形成「外层原因：内层原因」的链条。
  /// 用法：`return std::unexpected(std::move(e).WithContext(Tr(Msg::kNetOpenBpfFailed)));`
  ///
  /// 分隔符随语言变化（中文是全角冒号、英文是 ": "），所以不能写死在这里。
  [[nodiscard]] Error WithContext(std::string_view outer) && {
    context_ = std::string{outer} + std::string{detail::ContextSeparator()} + context_;
    return std::move(*this);
  }

  /// 渲染成形如 `声明数据接口失败 [libusb: LIBUSB_ERROR_ACCESS(-3)]` 的可读串。
  [[nodiscard]] std::string ToString() const;

 private:
  Error(ErrorDomain domain, std::int64_t code, std::string context)
      : code_(code), context_(std::move(context)), domain_(domain) {}

  std::int64_t code_ = 0;
  std::string context_;
  ErrorDomain domain_ = ErrorDomain::kGeneric;
};

/// 可能失败并返回值的操作。
template <typename T>
using Result = std::expected<T, Error>;

/// 可能失败但无返回值的操作。
using Status = std::expected<void, Error>;

/// 成功的 Status 字面量。
inline Status Ok() noexcept {
  return Status{};
}

namespace detail {

// 用于生成唯一临时变量名，避免宏展开时的变量遮蔽。
#define TETHERKITNEXT_CONCAT_INNER(a, b) a##b
#define TETHERKITNEXT_CONCAT(a, b) TETHERKITNEXT_CONCAT_INNER(a, b)
#define TETHERKITNEXT_UNIQUE(base) TETHERKITNEXT_CONCAT(base, __LINE__)

}  // namespace detail

/// 若表达式失败则立即向上传播错误。
///
/// 用法：`TETHERKITNEXT_RETURN_IF_ERROR(link.Configure(mtu));`
#define TETHERKITNEXT_RETURN_IF_ERROR(expr)                                      \
  do {                                                                       \
    auto TETHERKITNEXT_UNIQUE(tk_status_) = (expr);                              \
    if (!TETHERKITNEXT_UNIQUE(tk_status_)) {                                     \
      return std::unexpected(std::move(TETHERKITNEXT_UNIQUE(tk_status_)).error()); \
    }                                                                        \
  } while (false)

/// 取出成功值赋给新变量，失败则向上传播错误。
///
/// 用法：`TETHERKITNEXT_ASSIGN_OR_RETURN(auto fd, OpenBpfDevice());`
///
/// `decl` 是一条声明而非表达式，不能加括号，故此处豁免 macro-parentheses 检查。
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define TETHERKITNEXT_ASSIGN_OR_RETURN(decl, expr)                              \
  auto TETHERKITNEXT_UNIQUE(tk_result_) = (expr);                               \
  if (!TETHERKITNEXT_UNIQUE(tk_result_)) {                                      \
    return std::unexpected(std::move(TETHERKITNEXT_UNIQUE(tk_result_)).error()); \
  }                                                                         \
  decl = std::move(TETHERKITNEXT_UNIQUE(tk_result_)).value()

}  // namespace tetherkitnext
