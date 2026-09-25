#include "capi_support.h"

#include <algorithm>
#include <ctime>

namespace tetherkitnext::capi {

std::int64_t WallNanos() noexcept {
  ::timespec now{};
  ::clock_gettime(CLOCK_REALTIME, &now);
  return (static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000) +
         static_cast<std::int64_t>(now.tv_nsec);
}

void ReconcileDeviceStrings(std::vector<RememberedDeviceStrings>& memory,
                            std::span<const DeviceIdentity> present,
                            std::span<tk_device_info_t> infos) {
  // 先淘汰已不在总线上的设备 —— 顺序重要：淘汰在回填之前，拔掉再插回
  // 同一地址的「另一台」设备才不会拿到前一台的名字。
  std::erase_if(memory, [present](const RememberedDeviceStrings& entry) {
    return std::ranges::find(present, entry.identity) == present.end();
  });

  for (tk_device_info_t& info : infos) {
    const bool read_succeeded =
        info.manufacturer[0] != '\0' || info.product[0] != '\0' || info.serial[0] != '\0';
    const DeviceIdentity identity = IdentityOf(info);
    const auto entry =
        std::ranges::find_if(memory, [identity](const RememberedDeviceStrings& candidate) {
          return candidate.identity == identity;
        });

    if (read_succeeded) {
      // 读到了 → 覆盖式记住（同身份重插时序列号也跟着最新一次读取走）。
      RememberedDeviceStrings& slot =
          entry != memory.end()
              ? *entry
              : memory.emplace_back(RememberedDeviceStrings{.identity = identity});
      CopyText(slot.manufacturer, info.manufacturer);
      CopyText(slot.product, info.product);
      CopyText(slot.serial, info.serial);
    } else if (entry != memory.end()) {
      // 没读到但有记忆 → 回填。设备真的没提供字符串时记忆本来就不存在，
      // 这里不会无中生有。
      CopyText(info.manufacturer, entry->manufacturer);
      CopyText(info.product, entry->product);
      CopyText(info.serial, entry->serial);
    }
  }
}

bool IsValidFethName(std::string_view name) noexcept {
  constexpr std::string_view kPrefix = "feth";
  // 名字还要塞得进内核的 IFNAMSIZ 缓冲。
  if (name.size() <= kPrefix.size() || name.size() >= TK_INTERFACE_NAME_CAPACITY) {
    return false;
  }
  if (!name.starts_with(kPrefix)) {
    return false;
  }
  return std::ranges::all_of(name.substr(kPrefix.size()),
                             [](char character) { return character >= '0' && character <= '9'; });
}

}  // namespace tetherkitnext::capi
