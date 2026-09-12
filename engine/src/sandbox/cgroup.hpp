// Session-level resource isolation via cgroup v2.
//
// Why it is needed (see the notes in exec/rlimit.hpp):
//   rlimits are mitigation rather than isolation, and RLIMIT_NPROC counts by
//   real UID, so it spills over onto the user's other processes. cgroup v2's
//   pids.max / memory.max apply only to that cgroup, which is what a genuine
//   per-sandbox quota looks like.
//
// The constraint that has to be handled -- cgroup v2's "no internal processes"
// rule:
//   A cgroup that already contains processes cannot enable controllers for its
//   children. In an ordinary user session hxd shares a scope with the shell,
//   and writing "+pids +memory" to that scope's cgroup.subtree_control gives
//   EBUSY (measured on the target machine). So this module must be able to
//   **probe** whether a child cgroup carrying real pids.max + memory.max can
//   be created at all, degrade cleanly when it cannot, and never pretend the
//   limits took effect.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>

namespace hx {

// Session-level cgroup limits. 0 means that entry is unlimited.
struct CgroupLimits {
  uint64_t max_pids = 0;         // pids.max
  uint64_t max_memory_bytes = 0; // memory.max
};

/**
 * Probe whether hxd can currently create a child cgroup carrying real
 * pids.max + memory.max.
 *
 * This is the one authoritative answer to "can limits really be applied": it
 * actually walks the full sequence
 *   enable the parent's controllers -> create the child cgroup -> write
 *   pids.max/memory.max
 * and returns false if any step fails (typically EBUSY because the parent
 * scope has internal processes). The temporary cgroup used for probing is
 * cleaned up before returning.
 *
 * On success returns true and writes an example usable child cgroup path to
 * *path; on failure returns false and writes the reason to *reason (so
 * --self-test can report honestly).
 */
bool Cgroup2Usable(std::string* path, std::string* reason);

/**
 * An RAII per-session cgroup: the constructor does nothing, Create makes the
 * cgroup and sets the limits, and the destructor removes the child cgroup. No
 * raw new/delete.
 */
class Cgroup2Session {
public:
  Cgroup2Session() = default;
  ~Cgroup2Session();

  Cgroup2Session(const Cgroup2Session&) = delete;
  Cgroup2Session& operator=(const Cgroup2Session&) = delete;

  // Create a child cgroup and write the limits into it. Returns true on
  // success (after which Active() is true). On failure returns false with *err
  // filled in, and this object is equivalent to never having been created.
  bool Create(const CgroupLimits& limits, std::string* err);

  // Move a pid into this cgroup (by writing cgroup.procs). Returns false with
  // *err filled in on failure.
  bool AddPid(pid_t pid, std::string* err);

  // Whether the limits were really established. When they were not, the caller
  // must treat it as "no cgroup isolation".
  bool Active() const { return active_; }
  const std::string& Path() const { return path_; }

private:
  std::string path_;
  bool active_ = false;
};

}  // namespace hx
