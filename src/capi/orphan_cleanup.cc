// 孤儿 feth 网卡的登记与清理。
//
// ★ 要解决的问题 ★
//   FethInterface 是 RAII 的，正常退出时网卡会被销毁。但**进程被 SIGKILL 时
//   析构根本不跑**，网卡就永远留在内核里了 —— 而 SIGKILL 是任何信号处理器都
//   拦不住的，所以「装个信号处理器」解决不了这件事。
//
//   唯一可靠的兜底是把创建过的接口名落盘，下次启动时清理。本文件就是那个落盘
//   与清理的实现。
//
// ★ 为什么不是「启动时销毁所有 feth」★
//   feth 是公共设施，别的程序（或用户手工 ifconfig）也可能在用。只销毁我们
//   自己登记过的，才不会误伤。
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capi_support.h"
#include "tetherkit/capi/tetherkit_c.h"
#include "tetherkit/common/i18n.h"
#include "tetherkit/common/logging.h"
#include "tetherkit/net/feth_device.h"

namespace {

using tetherkit::Msg;
using tetherkit::Tr;
using tetherkit::capi::ClearError;
using tetherkit::capi::FillGenericError;
using tetherkit::capi::IsValidFethName;

/// 登记文件路径。
///
/// 放 /var/run（= /private/var/run）而不是 /tmp：这里只有 root 可写，而能写
/// 这个文件就等于能让我们去销毁任意名字的接口 —— 虽然名字有 feth<数字> 的
/// 校验兜底，但把权限收紧成 root-only 才是正确的第一道防线。
///
/// 系统重启时 /var/run 会被清空，而重启也会顺带清掉所有 feth —— 两者的生命周期
/// 恰好一致，不需要额外处理陈旧条目。
constexpr const char* kRegistryPath = "/var/run/tetherkit-interfaces";

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
    // 去掉可能的尾随空白，再做一次名字校验 —— 文件内容永远当作不可信输入。
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
  // 空列表就把文件删掉，省得留一个空文件让人以为还有残留。
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

/// 安装给 net::SetInterfaceRegistry 的回调。
///
/// 每次创建/销毁都整读整写一遍文件。听起来浪费，但一次会话只创建两张网卡，
/// 而换成「只追加」就得处理销毁时的行删除与文件增长，复杂度不值得。
void OnInterfaceChanged(std::string_view name, bool created) noexcept {
  // 整个回调必须 noexcept —— 销毁路径可能在析构里。fstream 默认不抛异常
  // （没有 exceptions() 设置），所以这里只要不自己 throw 就是安全的。
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

}  // namespace

namespace tetherkit::capi {

void InstallInterfaceRegistry() {
  net::SetInterfaceRegistry(&OnInterfaceChanged);
}

}  // namespace tetherkit::capi

tk_result_t tk_cleanup_orphan_interfaces(size_t* out_removed, tk_error_t* out_error) {
  ClearError(out_error);
  if (out_removed != nullptr) {
    *out_removed = 0;
  }
  if (::geteuid() != 0) {
    FillGenericError(out_error, Tr(Msg::kCapiCleanupNeedsRoot));
    return TK_ERR_PERMISSION;
  }

  // ⚠️ 销毁循环里**绝不能**持有 RegistryMutex：DestroyInterfaceByName 成功后会
  // 触发登记回调，而回调要拿同一把锁 —— std::mutex 不可重入，持着进去就是当场
  // 自等死锁。所以这里把「读」「销毁」「收尾」拆成三段，锁只在头尾两段持有。
  std::vector<RegistryEntry> entries;
  {
    const std::lock_guard<std::mutex> guard(RegistryMutex());
    entries = ReadRegistry();
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
    // 销毁失败最常见的原因是接口已经不存在了（比如系统重启过），那正是我们
    // 想要的结果。真正的失败只记日志，不阻断其余条目。
    if (const auto status = tetherkit::net::DestroyInterfaceByName(entry.name); status) {
      ++removed;
      continue;
    }
    TETHERKIT_DEBUG_TR(Msg::kCapiOrphanAlreadyGone, entry.name);
  }

  // 收尾：能销毁的已经由回调逐行删掉了，剩下的是「本来就不在内核里」的条目，
  // 留着只会让下次启动重复尝试，一并清空 —— but keep entries that are still
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
