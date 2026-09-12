// The Linux definition of Confinement: a Landlock ruleset fd.
//
// ★ The ordering contract (get it wrong and there is no sandbox at all):
//     parent BuildConfinement() -> fork() -> child ApplyRestrictSelf() -> execve()
//   Landlock cannot revoke already-open fds, so it must be applied before
//   execve and inside the child.
#pragma once
#ifndef _WIN32

#include <unistd.h>

#include <string>

#include "sandbox/confine.hpp"

namespace hx {

struct Confinement {
  int fd = -1;
  ~Confinement() {
    if (fd >= 0) ::close(fd);
  }
};

// Called in the child after fork; returns false with err filled in.
// It calls prctl(PR_SET_NO_NEW_PRIVS) first, which landlock_restrict_self
// requires.
bool ApplyRestrictSelf(const Confinement* conf, std::string* err);

uint64_t FsAccessMaskForAbi(int abi);

}  // namespace hx

#endif  // !_WIN32
