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

  // Try the zero-copy fast path first: CFStringGetCStringPtr gives the pointer directly when the string is internally UTF-8 already,
  // which hits in the vast majority of cases (what we read are ASCII things like NIC names and IPs).
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
  // Function-local static: created on the first call and not released before process exit. See the header for the reason.
  //
  // Not locking is safe -- since C++11, the initialization of a function-local static is guaranteed thread-safe by the compiler
  // (magic static), and on concurrent first calls only one thread actually runs the initialization.
  static SCDynamicStoreRef store = ::SCDynamicStoreCreate(
      kCFAllocatorDefault, CFSTR("TetherKitNext"), /*callout=*/nullptr, /*context=*/nullptr);
  return store;
}

}  // namespace tetherkitnext::capi
