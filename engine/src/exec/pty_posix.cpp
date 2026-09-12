#ifndef _WIN32
#include "exec/pty.hpp"

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "sandbox/landlock.hpp"

namespace hx {

PtySpawnResult SpawnPty(const PtySpawnRequest& req) {
  PtySpawnResult r;
  if (req.argv.empty()) {
    r.error = "empty argv";
    return r;
  }

  struct winsize ws {};
  ws.ws_row = req.rows;
  ws.ws_col = req.cols;

  int master = -1;
  // forkpty creates the pty pair, calls setsid in the child, and wires the
  // slave end to 0/1/2
  const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) {
    r.error = std::string("forkpty: ") + ::strerror(errno);
    return r;
  }

  if (pid == 0) {
    // ---- child. The order of application is identical to the pipe flavour ----
    if (!req.cwd.empty() && ::chdir(req.cwd.c_str()) != 0) {
      ::fprintf(stderr, "hxd: chdir: %s\r\n", ::strerror(errno));
      ::_exit(126);
    }

    std::string err;
    if (!ApplyRestrictSelf(req.conf, &err)) {
      ::fprintf(stderr, "hxd: sandbox: %s\r\n", err.c_str());
      ::_exit(126);
    }
    if (!ApplyLimits(req.limits, &err)) {
      ::fprintf(stderr, "hxd: rlimit: %s\r\n", err.c_str());
      ::_exit(126);
    }
    // seccomp goes last, or it would block the steps above
    if (!ApplySeccomp(req.seccomp, &err)) {
      ::fprintf(stderr, "hxd: seccomp: %s\r\n", err.c_str());
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
    ::fprintf(stderr, "hxd: execvpe(%s): %s\r\n", cargv[0], ::strerror(errno));
    ::_exit(127);
  }

  // ---- parent ----
  r.ok = true;
  r.proc.pid = pid;  // forkpty already called setsid, so pgid == pid
  // On Linux the pty master is read and written through the same fd
  r.master_out = master;
  r.master_in = master;
  return r;
}

}  // namespace hx
#endif  // !_WIN32
