#ifdef _WIN32
#include "exec/pty.hpp"

#include <windows.h>

#include "exec/spawn.hpp"
#include "io/io_win.hpp"
#include "platform/win_util.hpp"

namespace hx {

// Launch lives in spawn_win.cpp and is shared by the pty and pipe paths --
// the same arrangement as "the security policy still goes through spawn.cpp"
// on the Linux side: the sandbox, the Job and the limits are applied in
// exactly one implementation, and the pty only wires up the two ends.
namespace win_spawn {
bool Launch(const std::vector<std::string>& argv, const std::string& cwd,
            const std::vector<std::string>& envp, const Confinement* conf, const Limits& limits,
            HANDLE child_in, HANDLE child_out, HANDLE child_err, HPCON pcon, Proc* proc,
            std::string* error);
}  // namespace win_spawn

namespace {

using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

/**
 * ★ Resolved dynamically rather than called directly.
 *
 *   ConPTY only exists from Windows 10 1809 (build 17763). Linked statically,
 *   the whole of hxd.exe **fails to load** on anything older -- even
 *   --self-test cannot run, so the "pty: available=false" line in caps never
 *   gets a chance to be printed. For probing to mean anything, its failure
 *   mode must not be "the program will not start".
 */
CreatePseudoConsoleFn GetCreate() {
  static CreatePseudoConsoleFn fn = [] {
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    return k32 == nullptr ? nullptr
                          : reinterpret_cast<CreatePseudoConsoleFn>(reinterpret_cast<void*>(
                                ::GetProcAddress(k32, "CreatePseudoConsole")));
  }();
  return fn;
}

ClosePseudoConsoleFn GetClose() {
  static ClosePseudoConsoleFn fn = [] {
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    return k32 == nullptr ? nullptr
                          : reinterpret_cast<ClosePseudoConsoleFn>(reinterpret_cast<void*>(
                                ::GetProcAddress(k32, "ClosePseudoConsole")));
  }();
  return fn;
}

}  // namespace

void ClosePtyHandle(void* pcon) {
  if (pcon == nullptr) return;
  ClosePseudoConsoleFn fn = GetClose();
  if (fn != nullptr) fn(static_cast<HPCON>(pcon));
}

PtySpawnResult SpawnPty(const PtySpawnRequest& req) {
  PtySpawnResult r;
  if (req.argv.empty()) {
    r.error = "empty argv";
    return r;
  }
  CreatePseudoConsoleFn create = GetCreate();
  if (create == nullptr) {
    // Report honestly that this machine does not have it, rather than quietly
    // falling back to pipe mode -- fall back and a REPL hangs waiting for a
    // tty, a symptom ten times harder to diagnose than a plain error.
    r.error = "ConPTY unavailable: requires Windows 10 1809 or later";
    return r;
  }

  // ConPTY requires the caller to supply two pipes: one it reads (the child's
  // input) and one it writes (the output). Our side of both must be overlapped
  // to join the event loop, so CreatePipePair is reused:
  //   parent_reads=false -> parent writable (feeds input), child end
  //                         synchronous (handed to ConPTY to read)
  //   parent_reads=true  -> parent readable (collects output), child end
  //                         synchronous (handed to ConPTY to write)
  io::Fd in_fd = io::kInvalid, out_fd = io::kInvalid;
  HANDLE conpty_reads = INVALID_HANDLE_VALUE;   // ConPTY reads the child's input from here
  HANDLE conpty_writes = INVALID_HANDLE_VALUE;  // ConPTY writes the child's output here

  auto cleanup = [&] {
    if (in_fd != io::kInvalid) io::Close(in_fd);
    if (out_fd != io::kInvalid) io::Close(out_fd);
    if (conpty_reads != INVALID_HANDLE_VALUE) ::CloseHandle(conpty_reads);
    if (conpty_writes != INVALID_HANDLE_VALUE) ::CloseHandle(conpty_writes);
  };

  if (!io::win::CreatePipePair(/*parent_reads=*/false, &in_fd, &conpty_reads, &r.error) ||
      !io::win::CreatePipePair(/*parent_reads=*/true, &out_fd, &conpty_writes, &r.error)) {
    cleanup();
    return r;
  }

  COORD size{};
  size.X = static_cast<SHORT>(req.cols);
  size.Y = static_cast<SHORT>(req.rows);
  HPCON pcon = nullptr;
  const HRESULT hr = create(size, conpty_reads, conpty_writes, 0, &pcon);
  if (FAILED(hr) || pcon == nullptr) {
    r.error = "CreatePseudoConsole: " + win::ErrorMessage(static_cast<DWORD>(hr));
    cleanup();
    return r;
  }

  Proc proc;
  const bool ok = win_spawn::Launch(req.argv, req.cwd, req.envp, req.conf, req.limits,
                                    INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
                                    INVALID_HANDLE_VALUE, pcon, &proc, &r.error);

  // ★ Close our copies of both ends whether this succeeded or failed.
  //   ConPTY has already duplicated each of them; leave ours open and the
  //   outputWrite side always has a holder, so the parent never sees EOF after
  //   the child exits -- and the cell is never done.
  //   Same reasoning as the parent having to close the far pipe ends after
  //   fork on Linux.
  ::CloseHandle(conpty_reads);
  conpty_reads = INVALID_HANDLE_VALUE;
  ::CloseHandle(conpty_writes);
  conpty_writes = INVALID_HANDLE_VALUE;

  if (!ok) {
    ClosePtyHandle(pcon);
    cleanup();
    return r;
  }

  proc.pty = pcon;  // lifetime handed to ReleaseProc: closed only after the process is reaped
  r.ok = true;
  r.proc = proc;
  r.master_out = out_fd;
  r.master_in = in_fd;
  return r;
}

}  // namespace hx
#endif  // _WIN32
