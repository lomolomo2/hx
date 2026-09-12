// Probing kernel isolation capabilities. The engine self-tests once at
// startup and writes the result into the rollout's session_meta -- so that
// reading the log later tells you whether that run actually had a sandbox.
#pragma once

#include <string>

#include "proto.hpp"

namespace hx {

struct Caps {
  // ★ This struct holds both platforms' probe results and is deliberately not
  //   split into two types. The reason is that session_meta's shape has to be
  //   stable: reading a rollout from three months ago, "did this run have a
  //   real sandbox" must be answerable from the same set of fields rather than
  //   requiring you to determine the platform first and then decide which keys
  //   to read. Unused fields keep their defaults, and CapsToJson emits only
  //   this platform's half.
  std::string kernel;  // Linux: uname -r; Windows: the version number
  int landlock_abi = 0;  // <= 0 means unavailable
  bool seccomp = false;
  bool userns = false;
  bool cgroup2 = false;
  std::string cgroup_path;
  // Whether cgroup2 can **actually** create a child cgroup carrying pids.max +
  // memory.max. `available` only says the filesystem is there; `usable` says
  // the limits can be applied (the no-internal-processes rule may block it).
  bool cgroup2_usable = false;
  // The reason when it is unusable. The engine neither pretends the limits
  // took effect nor hides why they did not.
  std::string cgroup2_reason;

  // ---- Windows ----
  //
  // The correspondences (each one is detailed in sandbox/confine_win.hpp and
  // exec/proc.hpp):
  //   appcontainer  <-  landlock + the network-blocking half of seccomp
  //   job_objects   <-  rlimit + cgroup + "the process group is ownership"
  //   conpty        <-  forkpty
  bool appcontainer = false;
  std::string appcontainer_reason;
  bool job_objects = false;
  bool nested_jobs = false;  // Win8+: a child creating its own Job still cannot escape ours
  bool conpty = false;
};

Caps DetectCaps();
json CapsToJson(const Caps& c);

/**
 * Whether this machine actually has a sandbox.
 *
 * --self-test's exit code rests on it (0 = yes, 2 = no). It is a function
 * rather than leaving callers to inspect fields because "what counts as a real
 * sandbox" asks about different fields on the two platforms (Linux looks at
 * landlock_abi, Windows at appcontainer), and that conclusion itself must have
 * exactly one definition -- it also decides whether the corresponding line in
 * session_meta is true.
 */
bool HasRealSandbox(const Caps& c);

// Landlock ABI to capability mapping (uapi/linux/landlock.h)
inline bool LandlockHasFs(int abi) { return abi >= 1; }
inline bool LandlockHasTruncate(int abi) { return abi >= 3; }
// TCP bind/connect restrictions exist from ABI 4 on. Note that Landlock does
// not cover UDP, so blocking the network still needs seccomp or a netns --
// see "risks and known limitations" in the plan.
inline bool LandlockHasNet(int abi) { return abi >= 4; }

}  // namespace hx
