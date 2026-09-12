#ifdef _WIN32
#include "exec/env.hpp"

#include <windows.h>

#include <vector>

#include "platform/platform.hpp"
#include "platform/win_util.hpp"

namespace hx {
namespace {

/**
 * The PATH inside the sandbox.
 *
 * ★ This single setting decides how usable the sandbox is, so the trade-off is
 *   worth spelling out.
 *
 *   The Linux side hardcodes
 *   "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin". That works
 *   because the FHS guarantees the toolchain lives in those directories.
 *   Windows has no FHS -- node / git / python install into Program Files,
 *   ProgramData, or anywhere at all, and PATH is the only thing pointing at
 *   them. Hardcoding a list would mean nothing runs inside the sandbox.
 *
 *   So the approach here is: **inherit the current PATH, but drop entries
 *   under the user profile**. That lines up exactly with the semantics on the
 *   Landlock side -- the first of the README's "known traps" says toolchains
 *   installed in $HOME by nvm / rustup / conda cannot run in the sandbox
 *   because $HOME is not readable by default. On Windows an AppContainer
 *   likewise cannot read the user profile, so leaving those entries on PATH
 *   only manufactures a harder-to-diagnose symptom: the command **is found**
 *   but fails to start (ACCESS_DENIED) instead of a clean "not found".
 *   Drop them and the failure mode matches Linux -- as does the fix: add the
 *   path to roots explicitly.
 */
std::string SandboxPath() {
  std::string raw;
  if (!platform::GetEnv("PATH", &raw)) raw.clear();
  const std::string home = platform::FoldCase(platform::HomeDir());

  std::vector<std::string> keep;
  size_t start = 0;
  while (start <= raw.size()) {
    const size_t semi = raw.find(';', start);
    const std::string entry =
        raw.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
    if (!entry.empty() && !platform::IsWithin(entry, home)) keep.push_back(entry);
    if (semi == std::string::npos) break;
    start = semi + 1;
  }

  // ★ System directories must come **first**, not merely "be present".
  //
  //   An inherited PATH very often has MSYS / Git-Bash / chocolatey bin
  //   directories on it, full of shims sharing names with system commands.
  //   When those sit ahead of System32, what runs in the sandbox is not the
  //   program the model thinks it is -- more dangerous than a missing command,
  //   because it **succeeds**, just with the wrong behaviour.
  //   (An earlier version only checked "is it already present", so System32
  //    was not hoisted because it appeared somewhere later on PATH; measured,
  //    "cmd" then resolved to a same-named msys script.)
  std::vector<std::string> head;
  std::string sysroot;
  if (platform::GetEnv("SystemRoot", &sysroot) && !sysroot.empty()) {
    const std::string sys32 = platform::Join(sysroot, "System32");
    head = {sys32, sysroot, platform::Join(sys32, "Wbem"),
            platform::Join(platform::Join(sys32, "WindowsPowerShell"), "v1.0"),
            platform::Join(sys32, "OpenSSH")};
  }

  std::string out;
  auto emit = [&out](const std::string& e) {
    if (!out.empty()) out.push_back(';');
    out += e;
  };
  for (const auto& h : head) emit(h);
  for (const auto& k : keep) {
    // Deduplicate: a hoisted system directory must not appear twice (a
    // trailing-separator spelling counts as the same one)
    std::string folded = platform::FoldCase(k);
    while (!folded.empty() && platform::IsSeparator(folded.back())) folded.pop_back();
    bool dup = false;
    for (const auto& h : head) {
      if (folded == platform::FoldCase(h)) {
        dup = true;
        break;
      }
    }
    if (!dup) emit(k);
  }
  return out;
}

/** Variables carried through from the current environment verbatim. Without
 *  them a great many Win32 APIs fail in strange ways. */
void CopyThrough(const char* name, std::vector<std::string>* env) {
  std::string v;
  if (platform::GetEnv(name, &v)) env->push_back(std::string(name) + "=" + v);
}

}  // namespace

std::vector<std::string> BuildMinimalEnv(const std::string& home, const std::string& tmpdir,
                                         const std::map<std::string, std::string>& overrides) {
  std::vector<std::string> env;
  env.push_back("PATH=" + SandboxPath());
  env.push_back("PATHEXT=.COM;.EXE;.BAT;.CMD");
  // The Linux side sets TERM=dumb so tools stop emitting ANSI control
  // sequences that pollute the context. On Windows this pair is the
  // counterpart: it turns off .NET / PowerShell coloured output.
  env.push_back("TERM=dumb");
  env.push_back("NO_COLOR=1");

  // ★ These are not optional.
  //   Without SystemRoot, winsock, the crypto APIs, and even cmd.exe itself
  //   crash or misbehave; without COMSPEC, any system() or batch invocation
  //   fails. The Linux side needs no counterpart, because there is no such
  //   thing there as "find the system directories via an environment
  //   variable".
  CopyThrough("SystemRoot", &env);
  CopyThrough("SystemDrive", &env);
  CopyThrough("windir", &env);
  CopyThrough("COMSPEC", &env);
  CopyThrough("NUMBER_OF_PROCESSORS", &env);
  CopyThrough("PROCESSOR_ARCHITECTURE", &env);

  // ★ LOCALAPPDATA is a **hard prerequisite** for an AppContainer process,
  //   not a convenience.
  //
  //   An AppContainer's profile lives under %LOCALAPPDATA%\Packages\<name>\,
  //   and CreateProcess resolves that directory from this variable while
  //   building the container. Without it CreateProcess simply fails, and with
  //   a thoroughly misleading error at that: ERROR_ENVVAR_NOT_FOUND(203),
  //   "The system could not find the environment option that was entered" --
  //   nothing in the code mentions AppContainer. Tracking it down took
  //   bisecting variable by variable.
  //   (It alone is required; APPDATA / USERPROFILE / ProgramData and friends
  //    are not.)
  //
  //   Pointing it at the real user directory does **not** weaken isolation:
  //   the variable existing is not the same as the path being readable. Under
  //   %LOCALAPPDATA% an AppContainer can only enter its own package
  //   subdirectory; everything else is still ACCESS_DENIED -- the same
  //   situation as "HOME has a value but Landlock does not allow it" on Linux.
  CopyThrough("LOCALAPPDATA", &env);

  // Same as the Linux side: HOME points at the workspace root, not the real
  // user directory. The sandbox cannot read the real profile anyway (the
  // AppContainer is not granted it), so pointing there would only send tools
  // to write to a path that is certain to fail.
  if (!home.empty()) {
    env.push_back("HOME=" + home);
    env.push_back("USERPROFILE=" + home);
  }

  // Compilers, bundlers and plenty of other tools write temp files. Windows
  // reads TEMP/TMP, not TMPDIR -- get the name wrong and tools fall back to
  // C:\Windows\Temp (which the sandbox has no rights to), so build tasks fail
  // for no visible reason.
  if (!tmpdir.empty()) {
    env.push_back("TEMP=" + tmpdir);
    env.push_back("TMP=" + tmpdir);
    env.push_back("TMPDIR=" + tmpdir);  // cross-platform tools (node, python) read this too
  }

  for (const auto& [k, v] : overrides) env.push_back(k + "=" + v);
  return env;
}

}  // namespace hx
#endif  // _WIN32
