// Resource limits.
//
// Why not cgroup v2:
//   cgroups require the parent cgroup to enable the controller first
//   (subtree_control), and cgroup v2's "no internal processes" rule forbids a
//   cgroup that already contains processes from enabling subtree controllers.
//   In an ordinary user session (hxd sharing a scope with the shell) that step
//   is guaranteed to return EBUSY. Working around it means moving the engine
//   into its own scope (systemd-run --scope -p Delegate=yes), which is a
//   deployment choice and not the engine's to make unilaterally.
//
// rlimits, by contrast, need no privileges and always work. Their boundary,
// stated honestly:
//   ★ RLIMIT_NPROC counts by "real UID", and it counts **threads (tasks), not
//     processes**. Measured on this machine: 104 processes correspond to 667
//     threads (background thread pools like tokio/gmain/gdbus dominate). So an
//     absolute value must never be hardcoded -- 256 was hardcoded once, and
//     the result was that every fork inside the sandbox failed while the
//     escape tests stayed green (they only verified "the limit was applied",
//     never "ordinary work still runs").
//     The correct approach: limit = current usage + headroom.
//     It is still mitigation rather than isolation; a real per-sandbox quota
//     needs cgroup pids.max.
#pragma once

#include <cstdint>
#include <string>

namespace hx {

struct Limits {
  // 0 means no limit
  uint64_t max_processes = 0;   // RLIMIT_NPROC (an absolute value, which LimitsFor should compute)
  uint64_t max_file_bytes = 0;  // RLIMIT_FSIZE: stops the disk being filled
  uint64_t max_open_files = 0;  // RLIMIT_NOFILE
  bool disable_core_dumps = true;

  /**
   * Memory limit for the whole subtree, in bytes. 0 = no limit.
   *
   * ★ Currently **inert on Linux**: this corresponds to a cgroup's memory.max,
   *   and as the README says plainly, Cgroup2Session is not wired into the
   *   spawn path yet. setrlimit has no equivalent (RLIMIT_AS limits address
   *   space rather than resident memory, which is close to useless for modern
   *   runtimes).
   *   Live on Windows: JOB_OBJECT_LIMIT_JOB_MEMORY is a real per-Job limit.
   *   The field lives here rather than being hidden in a Windows-private
   *   struct so that the asymmetry is visible in the types.
   */
  uint64_t max_job_memory_bytes = 0;
};

#ifndef _WIN32
/** Count this UID's current tasks (threads, not processes). 0 on failure. */
uint64_t CountUserTasks(unsigned uid);
#endif

/**
 * Compute the limits available to this session: max_processes = current task
 * count + headroom.
 *
 * When current usage cannot be counted, **no NPROC limit is set** rather than
 * falling back to a guess -- guessing low makes the entire sandbox unusable,
 * whereas a fork bomb still has the timeout plus killing the process group as
 * a backstop.
 */
Limits LimitsFor(uint64_t headroom = 512);

/** Equivalent to LimitsFor(); kept for callers that do not care about headroom. */
Limits DefaultLimits();

#ifndef _WIN32
// Called after fork and before execve. Returns false with err filled in.
bool ApplyLimits(const Limits& l, std::string* err);
#else
/**
 * Apply the limits to the Job object; call **before**
 * AssignProcessToJobObject.
 *
 * ★ Why the RLIMIT_NPROC trap simply cannot recur on Windows:
 *   JOB_OBJECT_LIMIT_ACTIVE_PROCESS counts **processes in this Job** -- not
 *   system-wide, not by UID, and certainly not threads. So an absolute value
 *   is fine: it constrains this one cell and is unaffected by however much
 *   else is running on the machine.
 *   The README's relative "current usage + 512" arithmetic is a Linux-specific
 *   patch, and neither should nor needs to be copied over.
 */
bool ApplyLimitsToJob(void* job, const Limits& l, std::string* err);
#endif

}  // namespace hx
