// A process and "the subtree it owns".
//
// ★ This is the hardest part of a cell's lifecycle, and the two platforms
//   answer it very differently, so a seam is mandatory.
//
//   Linux: the child calls setsid() to become its own process group, and
//          reaping works by "freeze + sweep repeatedly" via kill(-pgid) --
//          because SIGKILL is in a race with fork, and nothing notifies you
//          when the group finally empties; the only way to tell is to probe
//          with kill(-pgid, 0).
//
//   Windows: Job objects. The subtree belongs to the Job by construction
//          (nested Jobs are supported from Win8 on, so a child creating its
//          own Job still cannot get out), and TerminateJobObject is
//          **atomic** -- no race, no freezing, no sweeping. On top of that,
//          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE guarantees the whole tree goes
//          down when hxd itself is killed, which the Linux side cannot do.
//
//   The `bash -c 'cmd &'` lesson in the README's "known traps" -- background
//   processes leaking out -- cannot repeat on Windows: the direct child
//   exiting and the Job being empty are two independent facts, and the engine
//   has always asked about the latter.
#pragma once

#include <cstdint>
#include <string>

namespace hx {

/**
 * The real process behind a cell.
 *
 * ★ A pid never appears in the protocol (the HANDLE model; see cell.hpp). It
 *   is kept here only for logging and diagnostics -- the moment it leaks into
 *   hxp/0, swapping the implementation for a container or a remote machine
 *   would mean changing the protocol.
 */
struct Proc {
  int64_t pid = -1;
  void* handle = nullptr;  // Windows: process HANDLE. Linux: unused
  void* group = nullptr;   // Windows: Job HANDLE. Linux: unused (pgid == pid)
  void* pty = nullptr;     // Windows: the ConPTY HPCON. Linux: unused (a pty is just an fd)

  bool valid() const { return pid > 0; }
};

/** The signal names the protocol allows. On Windows only kKill is exact; for
 *  the rest see proc_win.cpp. */
enum class Sig { kTerm, kKill, kInt, kHup, kQuit };

bool ParseSignal(const std::string& name, Sig* out);

/** Is any process in this subtree still alive? (Not "is the direct child
 *  alive" -- those are two different questions.) */
bool GroupAlive(const Proc& p);

/** Non-blocking reap of the direct child. Returns true once it has exited and
 *  fills in the exit code / terminating signal. */
bool Reap(Proc* p, int* exit_code, int* term_signal);

/** Signal the whole subtree. Returns false when it could not be delivered --
 *  never silently escalates to a hard kill. */
bool SignalGroup(const Proc& p, Sig s);

/**
 * Forcibly take down the whole subtree.
 *
 * Linux:   SIGSTOP to freeze, then SIGKILL (a stopped process cannot fork, so
 *          there is no race against its breeding rate), and **never**
 *          SIGCONT -- thawing lets anything missed this round start breeding
 *          again.
 * Windows: TerminateJobObject, done in one shot.
 */
void FreezeAndKillGroup(const Proc& p);

/**
 * Duplicate the group handle for "repeated sweeping", handing ownership to
 * the caller.
 *
 * ★ Returning false is not a failure -- it means "this platform needs no
 *   sweeping":
 *     Linux   true  -- SIGKILL races fork, one pass does not finish the job,
 *                      and the cell was reaped by exec.wait long ago, so the
 *                      cleanup has to outlive it.
 *     Windows false -- the kernel locks the entire Job for the duration of
 *                      TerminateJobObject, so no survivor can fork; it lands
 *                      in one shot and there is nothing left to sweep.
 */
bool DupGroupForSweep(const Proc& p, Proc* out);

/** Release the kernel objects held. Essentially a no-op on Linux. */
void ReleaseProc(Proc* p);

}  // namespace hx
