#include "tetherkit/common/i18n.h"
#include "process_runner.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <format>
#include <memory>

namespace tetherkit::capi {
namespace {

/// 读满一个管道直到对端关闭。
std::string ReadAll(int fd) {
  std::string content;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = ::read(fd, buffer.data(), buffer.size());
    if (count > 0) {
      content.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    // count == 0 是 EOF；EINTR 要重试，其余错误当作读完。
    if (count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  return content;
}

Result<ProcessResult> Spawn(std::string_view executable,
                            const std::vector<std::string>& arguments) {
  // ---- 组装 argv ----
  //
  // posix_spawn 要 char* const[]，而我们只有 const 字符串。这些指针在
  // posix_spawn 返回前不会被修改，按 POSIX 规定这样传是合法的。
  const std::string executable_path{executable};
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(const_cast<char*>(executable_path.c_str()));  // NOLINT
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));  // NOLINT
  }
  argv.push_back(nullptr);

  // ---- 建管道收输出 ----
  std::array<int, 2> pipe_fds{-1, -1};
  if (::pipe(pipe_fds.data()) != 0) {
    return std::unexpected(Error::FromErrno(0, Tr(Msg::kCapiPipeFailed)));
  }

  ::posix_spawn_file_actions_t actions{};
  if (const int rc = ::posix_spawn_file_actions_init(&actions); rc != 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return std::unexpected(Error::FromErrno(rc, Tr(Msg::kCapiSpawnFileActionsFailed)));
  }
  // 子进程：关掉读端，把写端接到 stdout 与 stderr，然后关掉原始写端。
  ::posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

  // Run system tools with a fixed, minimal environment instead of inheriting
  // ours. This code runs as root; an inherited environment (DYLD_*, locale
  // overrides, TMPDIR, ...) is attacker-influenced input for no benefit —
  // ipconfig/route need nothing from it. LANG=C also keeps their error output
  // stable for the messages we surface.
  static char kPathVar[] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
  static char kLangVar[] = "LANG=C";
  std::array<char*, 3> clean_environment{kPathVar, kLangVar, nullptr};

  ::pid_t child = -1;
  const int spawn_rc = ::posix_spawn(&child, executable_path.c_str(), &actions, nullptr,
                                     argv.data(), clean_environment.data());
  ::posix_spawn_file_actions_destroy(&actions);
  // 父进程必须立刻关掉写端，否则 ReadAll 永远等不到 EOF —— 自己还握着一个
  // 写端，管道就不会关。这是最经典的 pipe 死锁。
  ::close(pipe_fds[1]);

  if (spawn_rc != 0) {
    ::close(pipe_fds[0]);
    return std::unexpected(
        Error::FromErrno(spawn_rc, Tr(Msg::kCapiExecFailed, executable_path)));
  }

  // 必须**先读空管道再 waitpid**：反过来的话，子进程写满管道缓冲（64 KiB）后
  // 会阻塞在 write 上，而我们阻塞在 waitpid 上，双方都不动。
  ProcessResult result;
  result.output = ReadAll(pipe_fds[0]);
  ::close(pipe_fds[0]);

  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return std::unexpected(
          Error::FromErrno(0, Tr(Msg::kCapiWaitFailed, executable_path)));
    }
  }

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    // 被信号打死：用 128+signo 表示，和 shell 的惯例一致，便于对着日志排查。
    result.exit_code = 128 + WTERMSIG(status);
  } else {
    result.exit_code = -1;
  }
  return result;
}

}  // namespace

Result<ProcessResult> RunTool(std::string_view executable,
                              std::initializer_list<std::string_view> arguments) {
  std::vector<std::string> owned;
  owned.reserve(arguments.size());
  for (const std::string_view argument : arguments) {
    owned.emplace_back(argument);
  }
  return Spawn(executable, owned);
}

Result<ProcessResult> RunTool(std::string_view executable,
                              const std::vector<std::string>& arguments) {
  return Spawn(executable, arguments);
}

}  // namespace tetherkit::capi
