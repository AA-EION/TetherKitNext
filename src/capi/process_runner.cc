#include "tetherkitnext/common/i18n.h"
#include "process_runner.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <format>
#include <memory>

namespace tetherkitnext::capi {
namespace {

/// Reads a pipe until the peer closes it.
std::string ReadAll(int fd) {
  std::string content;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = ::read(fd, buffer.data(), buffer.size());
    if (count > 0) {
      content.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    // count == 0 is EOF; EINTR must be retried, and other errors are treated as end of reading.
    if (count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  return content;
}

Result<ProcessResult> Spawn(std::string_view executable,
                            const std::vector<std::string>& arguments) {
  // ---- Assemble argv ----
  //
  // posix_spawn wants char* const[], while we only have const strings. These pointers are not modified before
  // posix_spawn returns, so passing them this way is legal per POSIX.
  const std::string executable_path{executable};
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(const_cast<char*>(executable_path.c_str()));  // NOLINT
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));  // NOLINT
  }
  argv.push_back(nullptr);

  // ---- Create pipes to collect output ----
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
  // Child process: close the read end, connect the write end to stdout and stderr, then close the original write end.
  // stdin comes from /dev/null rather than whatever the daemon inherited.
  ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
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

  // POSIX_SPAWN_CLOEXEC_DEFAULT (Darwin): the child inherits only the
  // descriptors the file actions above set up (stdin/stdout/stderr). Without
  // it every descriptor this root process holds that lacks FD_CLOEXEC -- the
  // BPF device, feth control sockets, libusb's pipes, the XPC listener's
  // descriptors -- would leak into ipconfig / route.
  ::posix_spawnattr_t attributes{};
  if (const int rc = ::posix_spawnattr_init(&attributes); rc != 0) {
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return std::unexpected(Error::FromErrno(rc, Tr(Msg::kCapiSpawnAttributesFailed)));
  }
  // NOLINTNEXTLINE(google-runtime-int): posix_spawnattr_setflags takes a short.
  ::posix_spawnattr_setflags(&attributes, static_cast<short>(POSIX_SPAWN_CLOEXEC_DEFAULT));

  ::pid_t child = -1;
  const int spawn_rc = ::posix_spawn(&child, executable_path.c_str(), &actions, &attributes,
                                     argv.data(), clean_environment.data());
  ::posix_spawnattr_destroy(&attributes);
  ::posix_spawn_file_actions_destroy(&actions);
  // The parent process must close the write end immediately, otherwise ReadAll will never see EOF -- it is itself still holding a
  // write end, so the pipe never closes. This is the classic pipe deadlock.
  ::close(pipe_fds[1]);

  if (spawn_rc != 0) {
    ::close(pipe_fds[0]);
    return std::unexpected(
        Error::FromErrno(spawn_rc, Tr(Msg::kCapiExecFailed, executable_path)));
  }

  // Must **drain the pipe first and then waitpid**: the other way around, the child fills the pipe buffer (64 KiB)
  // and blocks on write, while we block on waitpid, and neither side moves.
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
    // Killed by a signal: expressed as 128+signo, consistent with shell convention, making it easy to investigate against the logs.
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

}  // namespace tetherkitnext::capi
