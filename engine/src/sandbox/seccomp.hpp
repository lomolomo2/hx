// The seccomp-bpf filter.
//
// It exists for exactly one reason: to cover what Landlock cannot reach.
//   - Landlock ABI 4 only covers TCP bind/connect -- UDP is entirely outside
//     its field of view, so a net:deny session could still send UDP outbound
//     (DNS being the most convenient channel).
//   - A further set of syscalls has nothing to do with "running a build" but
//     is well suited to breaking out: ptrace / mount / pivot_root / keyctl /
//     bpf / loading kernel modules.
//
// ★ This layer only subtracts: it permits nothing, and merely seals off more
//   on top of Landlock. The two layers fail differently, which is what makes
//   stacking them worthwhile.
#pragma once

#include <string>

#include "sandbox/policy.hpp"

namespace hx {

struct SeccompPlan {
  bool block_inet = false;  // seal off socket() for AF_INET/AF_INET6, covering UDP
  bool block_admin = true;  // seal off ptrace/mount/keyctl/bpf and friends
};

#ifndef _WIN32
// Called after fork and before execve (the same place as Landlock).
// Returns false with err filled in -- the caller must treat that as fatal and
// never degrade to executing anyway.
bool ApplySeccomp(const SeccompPlan& plan, std::string* err);
#endif
// ★ This layer does **not** exist on Windows, and not in a "not done yet"
//   sense: seccomp exists on Linux to cover Landlock's UDP blind spot, and
//   when an AppContainer is not granted internetClient, WFP blocks UDP too, so
//   the blind spot does not exist in the first place.
//   Forcing in an empty implementation that returns true would turn
//   net_enforced in the log into a lie, so it is better for it not to exist on
//   Windows at all -- anyone who calls it fails to compile.

// Policy -> plan. A pure mapping, shared by both platforms (see policy.cpp).
SeccompPlan PlanFor(const Policy& p);

}  // namespace hx
