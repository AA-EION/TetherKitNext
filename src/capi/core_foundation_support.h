// Minimal RAII wrappers for CoreFoundation / SystemConfiguration.
//
// Wrapped only to the point of "enough": the only place in the project that needs to touch CF is reading and writing SCDynamicStore,
// so introducing a full CF smart-pointer library is not worthwhile, yet hand-written CFRelease is too easy to miss
// on early-return branches.
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <string>
#include <string_view>
#include <utility>

namespace tetherkitnext::capi {

/// Owns a CF object, calling CFRelease on destruction.
///
/// Used only for references **we own** under the Create/Copy rule. References returned under the Get rule, such as by CFDictionaryGetValue,
/// are not owned and must never be put in here -- that would cause an over-release.
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

/// UTF-8 string -> CFString.
[[nodiscard]] ScopedCFRef<CFStringRef> MakeCFString(std::string_view text);

/// CFString -> UTF-8 std::string. Passing nullptr yields an empty string.
[[nodiscard]] std::string CopyToStdString(CFStringRef text);

/// The process-wide shared SCDynamicStore handle. Created on the first call and reused ever after; returns
/// nullptr on failure.
///
/// * Why it must be process-wide and long-lived *
///   A value **set** in an SCDynamicStore by some session is deleted together when that session is released.
///   We need to write DNS keys into the dynamic store; if we used a store that is created temporarily and released after use,
///   the value we just wrote would vanish in an instant -- and the symptom is "the write returns success, the readback is empty",
///   which is very hard to track down.
///
///   The cost is that this handle is not released until the process exits, which is exactly what we want: when the helper exits,
///   the dynamic store entries it wrote are cleaned up automatically, leaving no garbage.
[[nodiscard]] SCDynamicStoreRef SharedDynamicStore();

}  // namespace tetherkitnext::capi
