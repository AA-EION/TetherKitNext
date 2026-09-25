// CoreFoundation / SystemConfiguration 的最小 RAII 封装。
//
// 只封到「够用」为止：项目里唯一需要碰 CF 的地方是读写 SCDynamicStore，
// 引入一个完整的 CF 智能指针库不划算，但手写 CFRelease 又太容易在提前 return
// 的分支上漏掉。
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <string>
#include <string_view>
#include <utility>

namespace tetherkitnext::capi {

/// 持有一个 CF 对象的所有权，析构时 CFRelease。
///
/// 只用于 Create/Copy 规则下**我们拥有**的引用。CFDictionaryGetValue 这类
/// Get 规则返回的引用不拥有所有权，绝不能塞进来 —— 会造成过度释放。
template <typename T>
class ScopedCFRef {
 public:
  ScopedCFRef() = default;
  explicit ScopedCFRef(T reference) noexcept : reference_(reference) {}

  ScopedCFRef(const ScopedCFRef&) = delete;
  ScopedCFRef& operator=(const ScopedCFRef&) = delete;

  ScopedCFRef(ScopedCFRef&& other) noexcept
      : reference_(std::exchange(other.reference_, nullptr)) {}

  ScopedCFRef& operator=(ScopedCFRef&& other) noexcept {
    if (this != &other) {
      Reset();
      reference_ = std::exchange(other.reference_, nullptr);
    }
    return *this;
  }

  ~ScopedCFRef() { Reset(); }

  [[nodiscard]] T Get() const noexcept { return reference_; }

  explicit operator bool() const noexcept { return reference_ != nullptr; }

 private:
  void Reset() noexcept {
    if (reference_ != nullptr) {
      ::CFRelease(reference_);
      reference_ = nullptr;
    }
  }

  T reference_ = nullptr;
};

/// UTF-8 字符串 → CFString。
[[nodiscard]] ScopedCFRef<CFStringRef> MakeCFString(std::string_view text);

/// CFString → UTF-8 std::string。传 nullptr 得到空串。
[[nodiscard]] std::string CopyToStdString(CFStringRef text);

/// 进程级共享的 SCDynamicStore 句柄。首次调用时创建，之后一直复用；失败返回
/// nullptr。
///
/// ★ 为什么必须是进程级长命的 ★
///   SCDynamicStore 里由某个会话**设置**的值，会在那个会话释放时被一并删除。
///   我们要往动态存储写 DNS 键，如果用临时创建、用完就 release 的 store，
///   刚写进去的值转眼就没了 —— 而且现象是「写入返回成功、读回来是空」，
///   非常难查。
///
///   代价是这个句柄直到进程退出都不释放，这正是我们要的：helper 退出时
///   由它写的动态存储条目自动清理，不留垃圾。
[[nodiscard]] SCDynamicStoreRef SharedDynamicStore();

}  // namespace tetherkitnext::capi
