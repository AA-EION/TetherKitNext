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
  // First evict devices no longer on the bus -- the order matters: eviction comes before backfilling, so that "another" device
  // that was unplugged and plugged back into the same address does not get the previous device's name.
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
      // Read -> remember by overwriting (when re-plugged with the same identity, the serial number also follows the latest read).
      RememberedDeviceStrings& slot =
          entry != memory.end()
              ? *entry
              : memory.emplace_back(RememberedDeviceStrings{.identity = identity});
      CopyText(slot.manufacturer, info.manufacturer);
      CopyText(slot.product, info.product);
      CopyText(slot.serial, info.serial);
    } else if (entry != memory.end()) {
      // Not read but there is a memory -> backfill. When a device truly provides no strings the memory never existed in the first place,
      // so nothing is conjured out of thin air here.
      CopyText(info.manufacturer, entry->manufacturer);
      CopyText(info.product, entry->product);
      CopyText(info.serial, entry->serial);
    }
  }
}

bool IsValidFethName(std::string_view name) noexcept {
  constexpr std::string_view kPrefix = "feth";
  // The name must also fit into the kernel's IFNAMSIZ buffer.
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
