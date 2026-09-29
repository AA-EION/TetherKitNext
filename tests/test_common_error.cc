// Unit tests of error.h / logging.h.
#include <cerrno>
#include <string>

#include <doctest.h>

#include "tetherkitnext/common/error.h"
#include "tetherkitnext/common/logging.h"

using tetherkitnext::Error;
using tetherkitnext::ErrorDomain;
using tetherkitnext::LogLevel;
using tetherkitnext::Ok;
using tetherkitnext::Result;
using tetherkitnext::Status;

namespace {

Status FailingStep() {
  return std::unexpected(Error::FromErrno(ENOENT, "打开 /dev/bpf0 失败"));
}

Status SucceedingStep() {
  return Ok();
}

/// Demonstrates the propagation chain of TETHERKITNEXT_RETURN_IF_ERROR.
Status PipelineWithFailure() {
  TETHERKITNEXT_RETURN_IF_ERROR(SucceedingStep());
  TETHERKITNEXT_RETURN_IF_ERROR(FailingStep());
  return Ok();
}

Result<int> ProducingStep(bool succeed) {
  if (!succeed) {
    return std::unexpected(Error::Generic("生产失败"));
  }
  return 42;
}

Result<int> ConsumingPipeline(bool succeed) {
  TETHERKITNEXT_ASSIGN_OR_RETURN(const int value, ProducingStep(succeed));
  return value * 2;
}

}  // namespace

TEST_SUITE("common.error") {

TEST_CASE("errno 域渲染出符号名与数值") {
  const Error error = Error::FromErrno(EACCES, "打开 BPF 设备失败");
  CHECK(error.Domain() == ErrorDomain::kErrno);
  CHECK(error.Code() == EACCES);
  const std::string text = error.ToString();
  CHECK(text.find("打开 BPF 设备失败") != std::string::npos);
  CHECK(text.find("errno") != std::string::npos);
  CHECK(text.find(std::to_string(EACCES)) != std::string::npos);
}

TEST_CASE("errno 传 0 时自动读取全局 errno") {
  errno = EPERM;
  const Error error = Error::FromErrno(0, "创建 feth 失败");
  CHECK(error.Code() == EPERM);
}

TEST_CASE("libusb 域渲染出 libusb 错误名") {
  // LIBUSB_ERROR_ACCESS == -3 is this project's most common failure (not running as root).
  const Error error = Error::FromLibUsb(-3, "声明 RNDIS 数据接口失败");
  CHECK(error.Domain() == ErrorDomain::kLibUsb);
  const std::string text = error.ToString();
  CHECK(text.find("LIBUSB_ERROR_ACCESS") != std::string::npos);
}

TEST_CASE("RNDIS 域输出原始状态码数值") {
  // tk_common **deliberately does not know** the symbolic names of RNDIS status codes -- that table exists in only
  // one place, rndis/protocol.cc (a single source, avoiding inconsistency between two places). So Error::ToString()
  // here is only responsible for outputting the hexadecimal value; the symbolic name is spliced into the context string by the rndis layer when it constructs the Error
  // (see MakeStatusError in rndis/messages.cc).
  const Error error = Error::FromRndisStatus(0xC00000BBU, "查询 OID 失败");
  const std::string text = error.ToString();
  CHECK(text.find("查询 OID 失败") != std::string::npos);
  CHECK(text.find("0xc00000bb") != std::string::npos);
  CHECK(error.Domain() == ErrorDomain::kRndis);
  CHECK(error.Code() == 0xC00000BB);
}

TEST_CASE("WithContext 形成外层到内层的原因链") {
  Error inner = Error::FromErrno(ENODEV, "ioctl(BIOCSETIF) 失败");
  const Error outer = std::move(inner).WithContext("绑定 BPF 到 feth1");
  const std::string text = outer.ToString();
  const auto outer_pos = text.find("绑定 BPF 到 feth1");
  const auto inner_pos = text.find("ioctl(BIOCSETIF) 失败");
  REQUIRE(outer_pos != std::string::npos);
  REQUIRE(inner_pos != std::string::npos);
  CHECK(outer_pos < inner_pos);  // the outer cause comes first
}

TEST_CASE("RETURN_IF_ERROR 传播首个失败") {
  const Status status = PipelineWithFailure();
  REQUIRE_FALSE(status.has_value());
  CHECK(status.error().Code() == ENOENT);
}

TEST_CASE("ASSIGN_OR_RETURN 成功时取值、失败时传播") {
  const Result<int> good = ConsumingPipeline(true);
  REQUIRE(good.has_value());
  CHECK(good.value() == 84);

  const Result<int> bad = ConsumingPipeline(false);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().Context() == "生产失败");
}

TEST_CASE("日志级别设置与判断") {
  const LogLevel original = tetherkitnext::GetLogLevel();

  tetherkitnext::SetLogLevel(LogLevel::kWarn);
  CHECK(tetherkitnext::GetLogLevel() == LogLevel::kWarn);
  CHECK_FALSE(tetherkitnext::detail::IsLogLevelEnabled(LogLevel::kInfo));
  CHECK(tetherkitnext::detail::IsLogLevelEnabled(LogLevel::kWarn));
  CHECK(tetherkitnext::detail::IsLogLevelEnabled(LogLevel::kError));

  tetherkitnext::SetLogLevel(LogLevel::kOff);
  CHECK_FALSE(tetherkitnext::detail::IsLogLevelEnabled(LogLevel::kError));

  tetherkitnext::SetLogLevel(original);
}

TEST_CASE("从 __FILE__ 截出文件名") {
  CHECK(tetherkitnext::detail::BaseName("/a/b/c/bpf_link.cc") == "bpf_link.cc");
  CHECK(tetherkitnext::detail::BaseName("bpf_link.cc") == "bpf_link.cc");
  CHECK(tetherkitnext::detail::BaseName("") == "");
}

}  // TEST_SUITE("common.error")
