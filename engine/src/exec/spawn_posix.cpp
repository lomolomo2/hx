#ifndef _WIN32
#include "exec/spawn.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace hx {
namespace {

// An error in the child can only _exit; it must not return
[[noreturn]] void ChildFail(const char* what, int code) {
  ::fprintf(stderr, "hxd: %s: %s\n", what, ::strerror(errno));
  ::_exit(code);
}

}  // namespace

SpawnCellResult SpawnCell(const SpawnCellRequest& req) {
  SpawnCellResult r;
  if (req.argv.empty()) {
    r.error = "empty argv";
    return r;
  }

  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  auto close_all = [&]() {
    for (int* p : {in_pipe, out_pipe, err_pipe}) {
      if (p[0] >= 0) ::close(p[0]);
      if (p[1] >= 0) ::close(p[1]);
    }
  };
  if (::pipe2(in_pipe, O_CLOEXEC) != 0 || ::pipe2(out_pipe, O_CLOEXEC) != 0 ||
      ::pipe2(err_pipe, O_CLOEXEC) != 0) {
    r.error = std::string("pipe2: ") + ::strerror(errno);
    close_all();
    return r;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    r.error = std::string("fork: ") + ::strerror(errno);
    close_all();
    return r;
  }

  if (pid == 0) {
    // ---- child ----
    // Its own process group: on timeout or kill the whole subtree can be
    // collected together (which is what stops a fork bomb escaping)
    if (::setsid() < 0) ChildFail("setsid", 126);

    if (::dup2(in_pipe[0], STDIN_FILENO) < 0) ChildFail("dup2(stdin)", 126);
    if (::dup2(out_pipe[1], STDOUT_FILENO) < 0) ChildFail("dup2(stdout)", 126);
    if (::dup2(err_pipe[1], STDERR_FILENO) < 0) ChildFail("dup2(stderr)", 126);
    // Every other pipe end carries O_CLOEXEC and closes automatically at execve

    if (!req.cwd.empty() && ::chdir(req.cwd.c_str()) != 0) ChildFail("chdir", 126);

    // ★ Order: chdir -> restrict_self -> execve
    std::string err;
    if (!ApplyRestrictSelf(req.conf, &err)) {
      ::fprintf(stderr, "hxd: sandbox: %s\n", err.c_str());
      ::_exit(126);  // if the sandbox cannot be applied, do not execute
    }
    if (!ApplyLimits(req.limits, &err)) {
      ::fprintf(stderr, "hxd: rlimit: %s\n", err.c_str());
      ::_exit(126);
    }
    // ★ seccomp must be applied last: it blocks a set of syscalls, and applied
    //   too early it would also block the chdir/dup2/setrlimit steps that
    //   follow.
    if (!ApplySeccomp(req.seccomp, &err)) {
      ::fprintf(stderr, "hxd: seccomp: %s\n", err.c_str());
      ::_exit(126);
    }

    std::vector<char*> cargv;
    cargv.reserve(req.argv.size() + 1);
    for (const auto& a : req.argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    std::vector<char*> cenv;
    cenv.reserve(req.envp.size() + 1);
    for (const auto& e : req.envp) cenv.push_back(const_cast<char*>(e.c_str()));
    cenv.push_back(nullptr);

    ::execvpe(cargv[0], cargv.data(), cenv.data());
    ChildFail("execvpe", 127);
  }

  // ---- parent: keep the ends that are ours, close the far ones ----
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  r.ok = true;
  r.proc.pid = pid;  // the child called setsid, so pgid == pid
  r.in_fd = in_pipe[1];
  r.out_fd = out_pipe[0];
  r.err_fd = err_pipe[0];
  return r;
}

SpawnResult RunForeground(const std::vector<std::string>& argv, const std::string& cwd,
                          const std::vector<std::string>& envp, const Confinement* conf,
                          const SeccompPlan& seccomp, const Limits& limits) {
  SpawnResult r;
  if (argv.empty()) {
    r.error = "empty argv";
    return r;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    r.error = std::string("fork: ") + ::strerror(errno);
    return r;
  }

  if (pid == 0) {
    // ---- child. Every failure past this point must _exit, never return ----
    //
    // ★ Order: chdir -> restrict_self -> execve
    //   Landlock cannot revoke already-open fds, so chdir has to finish before
    //   it is applied; and restrict_self must come before execve, or the new
    //   program runs under no constraints at all.
    if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
      ::fprintf(stderr, "hxd: chdir(%s): %s\n", cwd.c_str(), ::strerror(errno));
      ::_exit(126);
    }

    std::string err;
    if (!ApplyRestrictSelf(conf, &err)) {
      ::fprintf(stderr, "hxd: sandbox: %s\n", err.c_str());
      ::_exit(126);  // if the sandbox cannot be applied, do not execute --
                     // never degrade to running unprotected
    }
    if (!ApplyLimits(limits, &err)) {
      ::fprintf(stderr, "hxd: rlimit: %s\n", err.c_str());
      ::_exit(126);
    }
    if (!ApplySeccomp(seccomp, &err)) {
      ::fprintf(stderr, "hxd: seccomp: %s\n", err.c_str());
      ::_exit(126);
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) {
      cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);

    std::vector<char*> cenv;
    cenv.reserve(envp.size() + 1);
    for (const auto& e : envp) cenv.push_back(const_cast<char*>(e.c_str()));
    cenv.push_back(nullptr);

    ::execvpe(cargv[0], cargv.data(), cenv.data());
    ::fprintf(stderr, "hxd: execvpe(%s): %s\n", cargv[0], ::strerror(errno));
    ::_exit(127);
  }

  // ---- parent ----
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    r.error = std::string("waitpid: ") + ::strerror(errno);
    return r;
  }
  r.started = true;
  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.term_signal = WTERMSIG(status);
    r.exit_code = 128 + r.term_signal;
  }
  return r;
}

}  // namespace hx
#endif  // !_WIN32
