// For tests: pins the interface language within a scope and restores it on exit.
//
// * Why it is needed *
//
//   Every test case that asserts "the error message mentions XXX" depends on the current language. And the language is **process-wide
//   state**, whose default (English) may also be changed by other test cases -- without pinning, the success or failure of such cases
//   depends on doctest's execution order, the hardest kind of intermittent failure to track down.
//
//   So: whenever an assertion contains user-facing literal text, put a
//   `const ScopedLanguage guard{Language::kChinese};` at the start of the test case.
#pragma once

#include "tetherkitnext/common/i18n.h"

namespace tetherkitnext::testing {

/// Switches the language within a scope, and restores the value from before entry on destruction.
class ScopedLanguage {
 public:
  explicit ScopedLanguage(Language language) noexcept : previous_(GetLanguage()) {
    SetLanguage(language);
  }

  ScopedLanguage(const ScopedLanguage&) = delete;
  ScopedLanguage& operator=(const ScopedLanguage&) = delete;
  ScopedLanguage(ScopedLanguage&&) = delete;
  ScopedLanguage& operator=(ScopedLanguage&&) = delete;

  ~ScopedLanguage() { SetLanguage(previous_); }

 private:
  Language previous_;
};

}  // namespace tetherkitnext::testing
