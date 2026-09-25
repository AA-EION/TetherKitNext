#include "core_foundation_support.h"

#include <vector>

namespace tetherkitnext::capi {

ScopedCFRef<CFStringRef> MakeCFString(std::string_view text) {
  return ScopedCFRef<CFStringRef>{::CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),  // NOLINT
      static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8,
      /*isExternalRepresentation=*/static_cast<Boolean>(false))};
}

std::string CopyToStdString(CFStringRef text) {
  if (text == nullptr) {
    return {};
  }

  // 先试零拷贝的快路径：CFStringGetCStringPtr 在内部本来就是 UTF-8 时直接给
  // 指针，绝大多数情况（我们读的都是网卡名、IP 这类 ASCII）都能命中。
  if (const char* direct = ::CFStringGetCStringPtr(text, kCFStringEncodingUTF8);
      direct != nullptr) {
    return std::string{direct};
  }

  const CFIndex length = ::CFStringGetLength(text);
  const CFIndex capacity = ::CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::vector<char> buffer(static_cast<std::size_t>(capacity), '\0');
  if (::CFStringGetCString(text, buffer.data(), capacity, kCFStringEncodingUTF8) == 0) {
    return {};
  }
  return std::string{buffer.data()};
}

SCDynamicStoreRef SharedDynamicStore() {
  // 函数内静态量：首次调用时创建，进程退出前不释放。理由见头文件。
  //
  // 不加锁是安全的 —— C++11 起，函数内静态量的初始化由编译器保证线程安全
  // （magic static），并发首次调用只会有一个线程真正执行初始化。
  static SCDynamicStoreRef store = ::SCDynamicStoreCreate(
      kCFAllocatorDefault, CFSTR("TetherKitNext"), /*callout=*/nullptr, /*context=*/nullptr);
  return store;
}

}  // namespace tetherkitnext::capi
