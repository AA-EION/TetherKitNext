// 测试用：在一个作用域内把界面语言钉死，退出时还原。
//
// ★ 为什么需要它 ★
//
//   凡是断言「错误消息里提到了 XXX」的用例，都依赖当前语言。而语言是**进程级
//   状态**，默认值（英文）还可能被别的用例改掉 —— 不钉死的话，这类用例的成败
//   就取决于 doctest 的执行顺序，是最难查的那种间歇性失败。
//
//   所以：只要断言里出现了面向用户的字面文字，就在用例开头放一个
//   `const ScopedLanguage guard{Language::kChinese};`。
#pragma once

#include "tetherkitnext/common/i18n.h"

namespace tetherkitnext::testing {

/// 作用域内切换语言，析构时还原成进入前的值。
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
