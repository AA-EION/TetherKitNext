// Registration and cleanup of orphaned feth NICs.
//
// * The problem to solve *
//   FethInterface is RAII, and on normal exit the NIC is destroyed. But **when a process is SIGKILLed
//   destructors do not run at all**, and the NIC stays in the kernel forever -- and SIGKILL cannot be intercepted by any signal
//   handler, so "install a signal handler" cannot solve this.
//
//   The only reliable backstop is to persist the names of created interfaces to disk and clean up on the next launch. This file is the implementation
//   of that persistence and cleanup.
//
// * Why not "destroy all feth at startup" *
//   feth is a shared facility, and other programs (or the user manually running ifconfig) may be using it too. Destroying only the ones
//   we registered ourselves avoids collateral damage.
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capi_support.h"
#include "managed_network_service.h"
#include "tetherkitnext/capi/tetherkitnext_c.h"
#include "tetherkitnext/common/i18n.h"
#include "tetherkitnext/common/logging.h"
#include "tetherkitnext/net/feth_device.h"

namespace {

using tetherkitnext::Msg;
using tetherkitnext::Text;
using tetherkitnext::Tr;
using tetherkitnext::capi::ClearError;
using tetherkitnext::capi::FillGenericError;
using tetherkitnext::capi::IsValidFethName;

/// Path of the registry file.
///
/// Placed in /var/run (= /private/var/run) rather than /tmp: only root can write here, and being able to write
/// this file is equivalent to being able to make us destroy interfaces of arbitrary names -- although the feth<digits> name validation
/// is a backstop, tightening permissions to root-only is the correct first line of defense.
///
/// At system reboot /var/run is cleared, and a reboot also clears all feth along the way -- the lifecycles of the two
/// coincide exactly, so no extra handling of stale entries is needed.
constexpr const char* kRegistryPath = "/var/run/tetherkitnext-interfaces";

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

/// One registry line: interface name plus the PID of the process that created
/// it. Lines written by older versions carry no PID and parse as pid 0
/// ("owner unknown"), which cleanup treats as an orphan — the old behaviour.
struct RegistryEntry {
  std::string name;
  ::pid_t owner = 0;
};

[[nodiscard]] std::vector<RegistryEntry> ReadRegistry() {
  std::vector<RegistryEntry> entries;
  std::ifstream input{kRegistryPath};
  if (!input) {
    return entries;
  }
  std::string line;
  while (std::getline(input, line)) {
    // Strip possible trailing whitespace, then validate the name once more -- file content is always treated as untrusted input.
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    RegistryEntry entry;
    const std::size_t space = line.find(' ');
    entry.name = line.substr(0, space);
    if (space != std::string::npos) {
      const char* first = line.data() + space + 1;
      const char* last = line.data() + line.size();
      int pid = 0;
      if (const auto [ptr, ec] = std::from_chars(first, last, pid);
          ec == std::errc{} && ptr == last && pid > 0) {
        entry.owner = pid;
      }
    }
    if (IsValidFethName(entry.name)) {
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

void WriteRegistry(const std::vector<RegistryEntry>& entries) noexcept {
  // If the list is empty, delete the file, to avoid leaving an empty file that makes people think there are still leftovers.
  if (entries.empty()) {
    ::unlink(kRegistryPath);
    return;
  }
  std::ofstream output{kRegistryPath, std::ios::trunc};
  if (!output) {
    return;
  }
  for (const RegistryEntry& entry : entries) {
    output << entry.name << ' ' << entry.owner << '\n';
  }
}

/// Whether `pid` is a live process other than ourselves.
///
/// Cleanup must never destroy interfaces that belong to a *running* session in
/// another process (e.g. the legacy LaunchDaemon still alive while the new
/// SMAppService daemon starts, or two helper builds side by side). Before the
/// registry recorded owners, the second process's startup cleanup would tear
/// down the first one's live feth pair. PID reuse can only make us keep an
/// entry we could have removed — the safe direction.
[[nodiscard]] bool IsOtherLiveProcess(::pid_t pid) noexcept {
  if (pid <= 0 || pid == ::getpid()) {
    return false;
  }
  return ::kill(pid, 0) == 0 || errno == EPERM;
}

/// The callback installed for net::SetInterfaceRegistry.
///
/// Every create/destroy reads and rewrites the whole file. It sounds wasteful, but a session creates only two NICs,
/// and switching to "append only" would require handling line deletion on destroy and file growth, which is not worth the complexity.
void OnInterfaceChanged(std::string_view name, bool created) noexcept {
  // The whole callback must be noexcept -- the destroy path may be in a destructor. fstream does not throw by default
  // (no exceptions() setting), while the SystemConfiguration cleanup explicitly catches exceptions.
  {
    const std::lock_guard<std::mutex> guard(RegistryMutex());
    std::vector<RegistryEntry> entries = ReadRegistry();

    if (created) {
      for (const RegistryEntry& existing : entries) {
        if (existing.name == name) {
          return;
        }
      }
      entries.push_back(RegistryEntry{.name = std::string{name}, .owner = ::getpid()});
    } else {
      std::erase_if(entries, [name](const RegistryEntry& entry) { return entry.name == name; });
    }
    WriteRegistry(entries);
  }

  if (!created) {
    // When the NIC is gone, the network service registered for it should go too, otherwise a dead entry pointing to a nonexistent interface
    // would be left in "System Settings -> Network".
    try {
      if (const auto status = tetherkitnext::capi::RemoveManagedNetworkService(name); !status) {
        TETHERKITNEXT_WARN_TR(Msg::kCapiServiceRemoveFailed, name, status.error().ToString());
      }
    } catch (...) {
      TETHERKITNEXT_WARN_TR(Msg::kCapiServiceRemoveFailed, name, Text(Msg::kCapiCommandNoOutput));
    }
  }
}

}  // namespace

namespace tetherkitnext::capi {

void InstallInterfaceRegistry() {
  net::SetInterfaceRegistry(&OnInterfaceChanged);
}

}  // namespace tetherkitnext::capi

tk_result_t tk_cleanup_orphan_interfaces(size_t* out_removed, tk_error_t* out_error) {
  ClearError(out_error);
  if (out_removed != nullptr) {
    *out_removed = 0;
  }
  if (::geteuid() != 0) {
    FillGenericError(out_error, Tr(Msg::kCapiCleanupNeedsRoot));
    return TK_ERR_PERMISSION;
  }

  // WARNING: The destroy loop **must never** hold RegistryMutex: after DestroyInterfaceByName succeeds it
  // triggers the registration callback, and the callback takes the same lock -- std::mutex is not reentrant, and going in holding it is an instant
  // self-wait deadlock. So "read", "destroy" and "wrap-up" are split into three phases here, with the lock held only in the first and last.
  std::vector<RegistryEntry> entries;
  {
    const std::lock_guard<std::mutex> guard(RegistryMutex());
    entries = ReadRegistry();
  }

  // Setup:/Network/Service persists across processes and even reboots. Sweeping away the feth services marked TetherKitNext first
  // is what covers the case where the helper was SIGKILLed last time and the system then rebooted: by then
  // the interface registration in /var/run is gone, but the services in the network preferences may still remain.
  //
  // Skipped while another live process still owns registered interfaces: its
  // services are not stale, and deleting them would cut that session's
  // network. The next cleanup after it exits catches anything left behind.
  //
  // Failures are only logged: cleaning NICs matters far more than cleaning services, and one service that cannot be deleted must not block the whole round of backstop
  // cleanup (a leftover feth would keep occupying kernel resources).
  const bool another_session_alive = std::ranges::any_of(
      entries, [](const RegistryEntry& entry) { return IsOtherLiveProcess(entry.owner); });
  if (!another_session_alive) {
    if (const auto status = tetherkitnext::capi::RemoveAllManagedNetworkServices(); !status) {
      TETHERKITNEXT_WARN_TR(Msg::kCapiStaleServiceCleanupFailed, status.error().ToString());
    }
  }

  if (entries.empty()) {
    return TK_OK;
  }

  std::size_t removed = 0;
  std::vector<RegistryEntry> still_owned;
  for (const RegistryEntry& entry : entries) {
    if (IsOtherLiveProcess(entry.owner)) {
      // Belongs to a session that is still running elsewhere — not an orphan.
      still_owned.push_back(entry);
      continue;
    }
    // The most common reason for a destroy failure is that the interface no longer exists (for example the system was rebooted), which is exactly what we
    // want. Real failures are only logged and do not block the remaining entries.
    if (const auto status = tetherkitnext::net::DestroyInterfaceByName(entry.name); status) {
      ++removed;
      continue;
    }
    TETHERKITNEXT_DEBUG_TR(Msg::kCapiOrphanAlreadyGone, entry.name);
  }

  // Wrap-up: those that could be destroyed were already deleted row by row by the callback, and what remains are entries that "were not in the kernel to begin with",
  // and leaving them would only make the next launch retry repeatedly, so clear them all -- but keep entries that are still
  // owned by a live process.
  {
    const std::lock_guard<std::mutex> guard(RegistryMutex());
    WriteRegistry(still_owned);
  }

  if (out_removed != nullptr) {
    *out_removed = removed;
  }
  return TK_OK;
}
