#ifdef _WIN32
#include "exec/spawn.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <vector>

#include "io/io_win.hpp"
#include "platform/platform.hpp"
#include "platform/win_util.hpp"
#include "sandbox/confine_win.hpp"

namespace hx {
namespace win_spawn {

/**
 * argv -> command-line string.
 *
 * ★ Windows has no execve(argv[]): CreateProcess takes a single string, and
 *   the child splits it back apart itself (usually with CommandLineToArgvW).
 *   Which means **the quoting rules are this layer's security boundary**. Get
 *   them wrong and a filename in argv containing a space or a quote is split
 *   into two arguments -- precisely the class of injection hole the comments
 *   in grep.ts worry about, only happening on the Windows side rather than in
 *   a shell.
 *
 * The rules (the strict inverse of CommandLineToArgvW):
 *   - contains space / tab / quote, or is empty -> wrap the whole thing in quotes
 *   - backslashes preceding a quote must be doubled
 *   - an embedded quote gets one more backslash in front of it
 */
std::wstring ArgvToCommandLine(const std::vector<std::string>& argv) {
  std::wstring out;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i > 0) out.push_back(L' ');
    const std::wstring a = win::Widen(argv[i]);
    const bool need_quote =
        a.empty() || a.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!need_quote) {
      out += a;
      continue;
    }
    out.push_back(L'"');
    for (size_t j = 0; j < a.size(); ++j) {
      size_t slashes = 0;
      while (j < a.size() && a[j] == L'\\') {
        ++slashes;
        ++j;
      }
      if (j == a.size()) {
        out.append(slashes * 2, L'\\');  // trailing backslashes must double, or they escape the closing quote
        break;
      }
      if (a[j] == L'"') {
        out.append(slashes * 2 + 1, L'\\');
      } else {
        out.append(slashes, L'\\');
      }
      out.push_back(a[j]);
    }
    out.push_back(L'"');
  }
  return out;
}

/**
 * cmd.exe's command line has to be built separately -- ArgvToCommandLine
 * **must not** be used for it.
 *
 * ★ This is the nastiest rule difference on Windows:
 *   CreateProcess is handed one whole string, and the overwhelming majority of
 *   programs split it back into argv with CommandLineToArgvW -- under those
 *   rules an embedded quote is written \". But **cmd.exe does not use those
 *   rules**: it treats a backslash as an ordinary character and merely pairs
 *   quotes up.
 *
 *   So ["cmd","/c","powershell -Command \"...\""] assembled by the general
 *   rules leaves cmd looking at a pile of literal \" characters, and the
 *   command handed to powershell becomes a **string literal**. The observed
 *   symptom is powershell echoing the command text back and exiting 0, looking
 *   like it "ran fine" while doing nothing at all -- another silent failure.
 *
 *   The right approach is `/s`: with it, cmd strips only the first and last
 *   quote after /c and leaves everything between them untouched. So all we
 *   have to do is wrap the command in one layer of quotes, with no escaping of
 *   the inner quotes at all.
 */
bool IsCmdExe(const std::wstring& exe) {
  size_t slash = exe.find_last_of(L"\\/");
  std::wstring name = (slash == std::wstring::npos) ? exe : exe.substr(slash + 1);
  for (wchar_t& c : name) c = static_cast<wchar_t>(::towlower(c));
  return name == L"cmd.exe";
}

/** Returns true when it has been assembled under cmd.exe's rules; false means
 *  take the general path. */
bool BuildCmdExeCommandLine(const std::wstring& exe, const std::vector<std::string>& argv,
                            std::wstring* out) {
  if (!IsCmdExe(exe) || argv.size() < 3) return false;
  std::wstring sw = win::Widen(argv[1]);
  for (wchar_t& c : sw) c = static_cast<wchar_t>(::towlower(c));
  if (sw != L"/c" && sw != L"/k") return false;

  std::wstring body;
  for (size_t i = 2; i < argv.size(); ++i) {
    if (i > 2) body.push_back(L' ');
    body += win::Widen(argv[i]);
  }
  // cmd strips the outermost pair of quotes; when the command itself ends in a
  // quote that goes wrong, so separate them with a space.
  if (!body.empty() && body.back() == L'"') body.push_back(L' ');

  *out = L"cmd.exe /s " + sw + L" \"" + body + L"\"";
  return true;
}

/** A list of "K=V" -> CreateProcess's environment block (\0-separated,
 *  terminated by a double \0). */
std::wstring BuildEnvBlock(const std::vector<std::string>& envp) {
  // Windows requires the environment block to be sorted by variable name,
  // case-insensitively. Leaving it unsorted works most of the time, but some
  // runtimes (including a few MSVC tools) binary-search it, and out of order
  // that shows up as "the variable is clearly set yet cannot be read".
  std::vector<std::wstring> items;
  items.reserve(envp.size());
  for (const auto& e : envp) items.push_back(win::Widen(e));
  std::sort(items.begin(), items.end(), [](const std::wstring& a, const std::wstring& b) {
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  std::wstring block;
  for (const auto& it : items) {
    block += it;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');  // even an empty environment needs the double \0
  return block;
}

/** Pull PATH out of envp (case-insensitively). */
std::wstring PathFromEnv(const std::vector<std::string>& envp) {
  for (const auto& e : envp) {
    const size_t eq = e.find('=');
    if (eq == std::string::npos) continue;
    std::string k = e.substr(0, eq);
    for (char& c : k) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (k == "path") return win::Widen(e.substr(eq + 1));
  }
  return std::wstring();
}

/**
 * Resolve the executable's absolute path.
 *
 * ★ Why not leave lpApplicationName null and let CreateProcess find it:
 *   it searches the **caller's** PATH (hxd's), while we deliberately gave the
 *   child a different, minimal environment. Left null, the PATH in envp is
 *   decorative -- what runs in the sandbox is a program from the host's PATH,
 *   which is not the same thing the policy describes. Searching envp's PATH
 *   ourselves is what makes the two agree.
 */
bool ResolveExecutable(const std::string& prog, const std::vector<std::string>& envp,
                       const std::string& cwd, std::wstring* out) {
  const std::wstring wprog = win::Widen(prog);
  const std::wstring path = PathFromEnv(envp);

  // Anything containing a separator resolves as a relative/absolute path and
  // does not consult PATH (matching execvp's behaviour)
  const bool has_sep = prog.find('/') != std::string::npos || prog.find('\\') != std::string::npos;

  // With a separator, resolve under cwd; otherwise search along envp's PATH.
  const std::wstring wcwd = win::Widen(cwd);
  const wchar_t* search_root = has_sep ? (wcwd.empty() ? nullptr : wcwd.c_str())
                                       : (path.empty() ? nullptr : path.c_str());

  // ★ Never try "no extension appended" first.
  //
  //   POSIX's execvp looks for an extensionless executable, and copying that
  //   over is wrong: on Windows an extensionless file is not executable at
  //   all, while PATH very often carries same-named shell scripts (MSYS and
  //   Git-Bash drop a pile of them into bin). Measured: "cmd" first hits the
  //   script C:\MinGW\msys\1.0\bin\cmd rather than System32\cmd.exe.
  //
  //   SearchPathW's lpExtension is only appended when "the filename itself has
  //   no extension", so passing the PATHEXT entries in order covers both
  //   spellings at once:
  //     "cmd"      -> completed to cmd.exe
  //     "node.exe" -> already has an extension, searched as-is
  const wchar_t* kExts[] = {L".exe", L".com", L".bat", L".cmd"};
  std::wstring buf(MAX_PATH, L'\0');
  for (const wchar_t* ext : kExts) {
    DWORD n = ::SearchPathW(search_root, wprog.c_str(), ext, static_cast<DWORD>(buf.size()),
                            buf.data(), nullptr);
    if (n >= buf.size()) {  // buffer too small; n is the required length
      buf.assign(static_cast<size_t>(n) + 1, L'\0');
      n = ::SearchPathW(search_root, wprog.c_str(), ext, static_cast<DWORD>(buf.size()),
                        buf.data(), nullptr);
    }
    if (n > 0 && n < buf.size()) {
      buf.resize(n);
      *out = buf;
      return true;
    }
    buf.assign(MAX_PATH, L'\0');
  }
  return false;
}

/** The part of CreateProcess shared by both paths. Fills in proc on success. */
bool Launch(const std::vector<std::string>& argv, const std::string& cwd,
            const std::vector<std::string>& envp, const Confinement* conf, const Limits& limits,
            HANDLE child_in, HANDLE child_out, HANDLE child_err, HPCON pcon, Proc* proc,
            std::string* error) {
  if (argv.empty()) {
    *error = "empty argv";
    return false;
  }

  std::wstring exe;
  if (!ResolveExecutable(argv[0], envp, cwd, &exe)) {
    *error = "executable not found on PATH: " + argv[0];
    return false;
  }
  std::wstring cmdline;
  // cmd.exe's quoting rules differ from CommandLineToArgvW's, so it has to be
  // assembled separately -- see BuildCmdExeCommandLine
  if (!BuildCmdExeCommandLine(exe, argv, &cmdline)) cmdline = ArgvToCommandLine(argv);
  std::wstring envblock = BuildEnvBlock(envp);
  // With HX_DEBUG_SPAWN=1, print verbatim what is actually handed to
  // CreateProcess. When diagnosing "the command runs in a shell but not in the
  // sandbox", the first thing to establish is what exe / cmdline / env really
  // are -- guessing is the slowest way there.
  if (std::string dbg; platform::GetEnv("HX_DEBUG_SPAWN", &dbg)) {
    std::fprintf(stderr, "hxd/debug: exe=[%s]\n", win::Narrow(exe).c_str());
    std::fprintf(stderr, "hxd/debug: cmdline=[%s]\n", win::Narrow(cmdline).c_str());
    std::fprintf(stderr, "hxd/debug: cwd=[%s]\n", cwd.c_str());
    for (const auto& e : envp) std::fprintf(stderr, "hxd/debug: env [%s]\n", e.c_str());
    std::fprintf(stderr, "hxd/debug: envblock wchars=%zu\n", envblock.size());
  }
  const std::wstring wcwd = win::Widen(cwd);

  // ---- Job: created and limited up front, so a process is fenced in the
  //      moment it joins ----
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    *error = win::LastError("CreateJobObject");
    return false;
  }
  if (!ApplyLimitsToJob(job, limits, error)) {
    ::CloseHandle(job);
    return false;
  }

  // ---- process attribute list ----
  SIZE_T attr_size = 0;
  // Attribute count: security capabilities (when sandboxed), the handle
  // allowlist (when not a pty), the pseudoconsole (when it is)
  DWORD attr_count = 1;
  if (conf != nullptr) ++attr_count;
  ::InitializeProcThreadAttributeList(nullptr, attr_count, 0, &attr_size);
  std::vector<char> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  if (::InitializeProcThreadAttributeList(attrs, attr_count, 0, &attr_size) == 0) {
    *error = win::LastError("InitializeProcThreadAttributeList");
    ::CloseHandle(job);
    return false;
  }
  auto attr_guard = std::unique_ptr<void, void (*)(void*)>(
      attrs, [](void* p) { ::DeleteProcThreadAttributeList(
                  reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(p)); });

  SECURITY_CAPABILITIES sec{};
  if (conf != nullptr) {
    // ★ This step is landlock_restrict_self's counterpart on Windows.
    //   The difference is timing: on Linux the child drops its own privileges
    //   after fork (so a wrong order leaves a window), while on Windows the
    //   parent fixes the token at process-creation time, before the child has
    //   executed its first instruction. There is no window to speak of, and
    //   therefore no "wrong order means it never happened" trap.
    if (!FillSecurityCapabilities(*conf, &sec)) {
      *error = "FillSecurityCapabilities failed";
      ::CloseHandle(job);
      return false;
    }
    if (::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sec,
                                    sizeof(sec), nullptr, nullptr) == 0) {
      *error = win::LastError("UpdateProcThreadAttribute(security capabilities)");
      ::CloseHandle(job);
      return false;
    }
  }

  HANDLE inherit[3];
  DWORD inherit_n = 0;
  if (pcon != nullptr) {
    // pty path: the pseudoconsole brings its own stdio and does not go through
    // the handle allowlist
    if (::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pcon,
                                    sizeof(pcon), nullptr, nullptr) == 0) {
      *error = win::LastError("UpdateProcThreadAttribute(pseudoconsole)");
      ::CloseHandle(job);
      return false;
    }
  } else {
    // ★ The handle allowlist is the counterpart of O_CLOEXEC.
    //   CreateProcess with bInheritHandles=TRUE hands over **every**
    //   inheritable handle. Without an explicit allowlist, another cell's pipe
    //   end can leak into this child -- conjuring a channel between two
    //   sandboxes that are supposed to be invisible to each other.
    if (child_in != INVALID_HANDLE_VALUE) inherit[inherit_n++] = child_in;
    if (child_out != INVALID_HANDLE_VALUE) inherit[inherit_n++] = child_out;
    if (child_err != INVALID_HANDLE_VALUE) inherit[inherit_n++] = child_err;
    if (inherit_n > 0 &&
        ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                    inherit_n * sizeof(HANDLE), nullptr, nullptr) == 0) {
      *error = win::LastError("UpdateProcThreadAttribute(handle list)");
      ::CloseHandle(job);
      return false;
    }
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.lpAttributeList = attrs;
  if (pcon == nullptr) {
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = child_err;
  }

  PROCESS_INFORMATION pi{};
  DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                // ★ Create it suspended. AssignProcessToJobObject has to land
                //   before the child starts running, or it gets a chance to
                //   fork grandchildren outside the limits before joining the
                //   Job.
                CREATE_SUSPENDED |
                // Its own process group: the prerequisite for SignalGroup
                // being able to deliver CTRL_BREAK, and it also keeps the
                // child from receiving Ctrl+C from our console.
                CREATE_NEW_PROCESS_GROUP;
  // ★ It must be CREATE_NO_WINDOW; DETACHED_PROCESS **must not** be used.
  //   This one was learned the hard way and is worth writing out in full.
  //
  //   Neither pops up a window; the difference is whether the child gets a
  //   console. With a console, the system also slips a conhost.exe into our
  //   Job -- and it outlives its client by a moment, so every ordinary command
  //   gets judged to have "left orphan processes behind". To dodge that, this
  //   was briefly changed to DETACHED_PROCESS (all three stdio streams are
  //   redirected anyway, so a console looked unnecessary).
  //
  //   The result was a **silent** collapse: cmd.exe itself still ran, and
  //   **builtins** like echo / type still produced output, but any **external**
  //   program cmd then started (node / ping / powershell) got no stdout at all
  //   -- exit code 0 or 1 and not one byte of output. Which is to say
  //   "run a command" looked fine while "run a real build command" was
  //   entirely broken. This is a replay of the README's lesson: the smoke
  //   cases happened to use only builtins, so the suite was green while the
  //   actual work was dead.
  //
  //   The conhost problem is solved elsewhere instead: GroupAlive excludes it
  //   by image path (see exec/proc_win.cpp), which is both precise and touches
  //   no execution path at all.
  if (pcon == nullptr) flags |= CREATE_NO_WINDOW;

  // ★ There is deliberately no "turn the sandbox off" debug switch here.
  //   Diagnostic switches should be read-only (HX_DEBUG_SPAWN only prints). A
  //   bypass that drops AppContainer via an environment variable is itself a
  //   privilege-escalation channel -- and exactly the kind most easily left
  //   behind in a production binary.
  if (::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr,
                       /*bInheritHandles=*/pcon == nullptr ? TRUE : FALSE, flags, envblock.data(),
                       wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi) == 0) {
    const DWORD e = ::GetLastError();
    *error = "CreateProcess(" + argv[0] + "): " + win::ErrorMessage(e);
    ::CloseHandle(job);
    return false;
  }

  if (::AssignProcessToJobObject(job, pi.hProcess) == 0) {
    // If it cannot join the Job, do not let it run -- the same principle as
    // "if the sandbox cannot be applied, do not execute": no Job means no
    // ownership of the process tree, and a single `start /b` leaves a
    // background process behind on the system.
    *error = win::LastError("AssignProcessToJobObject");
    ::TerminateProcess(pi.hProcess, 126);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(job);
    return false;
  }

  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);

  proc->pid = static_cast<int64_t>(pi.dwProcessId);
  proc->handle = pi.hProcess;
  proc->group = job;
  return true;
}

}  // namespace win_spawn

SpawnCellResult SpawnCell(const SpawnCellRequest& req) {
  SpawnCellResult r;
  if (req.argv.empty()) {
    r.error = "empty argv";
    return r;
  }
  // If the sandbox cannot be applied, do not execute. This invariant is
  // identical on both platforms.
  // (A null conf is only legal under danger-full-access, and the caller has
  //  already checked that.)

  io::Fd in_fd = io::kInvalid, out_fd = io::kInvalid, err_fd = io::kInvalid;
  HANDLE child_in = INVALID_HANDLE_VALUE, child_out = INVALID_HANDLE_VALUE,
         child_err = INVALID_HANDLE_VALUE;
  auto cleanup = [&] {
    for (io::Fd f : {in_fd, out_fd, err_fd}) {
      if (f != io::kInvalid) io::Close(f);
    }
    for (HANDLE h : {child_in, child_out, child_err}) {
      if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
  };

  if (!io::win::CreatePipePair(/*parent_reads=*/false, &in_fd, &child_in, &r.error) ||
      !io::win::CreatePipePair(/*parent_reads=*/true, &out_fd, &child_out, &r.error) ||
      !io::win::CreatePipePair(/*parent_reads=*/true, &err_fd, &child_err, &r.error)) {
    cleanup();
    return r;
  }

  if (!win_spawn::Launch(req.argv, req.cwd, req.envp, req.conf, req.limits, child_in, child_out,
                         child_err, nullptr, &r.proc, &r.error)) {
    cleanup();
    return r;
  }

  // The child's three ends have been handed over, so the parent closes its own
  // copies immediately -- leave them open and the pipes never reach EOF after
  // the child exits, so the cell is never done.
  ::CloseHandle(child_in);
  ::CloseHandle(child_out);
  ::CloseHandle(child_err);

  r.ok = true;
  r.in_fd = in_fd;
  r.out_fd = out_fd;
  r.err_fd = err_fd;
  return r;
}

SpawnResult RunForeground(const std::vector<std::string>& argv, const std::string& cwd,
                          const std::vector<std::string>& envp, const Confinement* conf,
                          const SeccompPlan& seccomp, const Limits& limits) {
  SpawnResult r;
  (void)seccomp;  // no such layer on Windows; see the notes in sandbox/seccomp.hpp
  if (argv.empty()) {
    r.error = "empty argv";
    return r;
  }

  Proc proc;
  // Foreground run: inherit hxd's own stdio directly, because the escape tests
  // need to see the real output
  if (!win_spawn::Launch(argv, cwd, envp, conf, limits, ::GetStdHandle(STD_INPUT_HANDLE),
                         ::GetStdHandle(STD_OUTPUT_HANDLE), ::GetStdHandle(STD_ERROR_HANDLE),
                         nullptr, &proc, &r.error)) {
    return r;
  }

  ::WaitForSingleObject(proc.handle, INFINITE);
  int code = -1, sig = 0;
  Reap(&proc, &code, &sig);
  r.started = true;
  r.exit_code = code;
  r.term_signal = sig;
  ReleaseProc(&proc);
  return r;
}

}  // namespace hx
#endif  // _WIN32
