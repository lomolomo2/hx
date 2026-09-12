// Host-side platform differences, concentrated in this one file.
//
// ★ This file **must not** touch the filesystem or start processes
//   (test/arch.sh enforces it). So it only answers questions of "shape": what
//   the engine binary is called, which shell a command is handed to, whether
//   two paths sit in the same tree. Anything that actually reaches the disk
//   goes over hxp to the engine.
//
//   node:process and node:path are pure computation and constitute no side
//   effect, so they are not on the forbidden list.
import { platform } from "node:process";
import { fileURLToPath } from "node:url";

export const isWindows = platform === "win32";

/**
 * The engine binary's default path.
 *
 * ★ Two traps live in this one line:
 *   1. Windows needs .exe, or spawn fails with ENOENT immediately.
 *   2. `new URL(...).pathname` cannot be used. On Windows it yields
 *      "/H:/myproject/hx/engine/build/hxd.exe" -- one leading slash too many,
 *      which is likewise ENOENT, while the error message makes the path look
 *      perfectly correct. fileURLToPath is the right way to turn a file: URL
 *      back into a local path.
 */
export function defaultEnginePath(relDir: string, baseUrl: string): string {
  const dir = new URL(relDir, baseUrl);
  return fileURLToPath(new URL(isWindows ? "hxd.exe" : "hxd", dir));
}

/**
 * Wrap a shell command into an argv the engine can execute.
 *
 * ★ Linux uses `bash -c` rather than `bash -lc`: a login shell reads
 *   ~/.profile, and $HOME is unreadable inside the sandbox, so every command
 *   would carry a line of "Permission denied" noise.
 *
 * ★ Windows defaults to `cmd /c`. This is not an arbitrary pick:
 *   PowerShell takes hundreds of milliseconds to start and loads the user
 *   profile (which the sandbox cannot read), while Git Bash may not be
 *   installed -- and the host has no fs, so it cannot probe for it.
 *   Pass HX_SHELL explicitly for a different shell; that is more honest than
 *   letting the host guess.
 */
export function shellCommand(cmd: string, shellOverride?: string): string[] {
  const shell = shellOverride?.trim();
  if (shell) {
    // "bash" / "pwsh" / "C:\\Program Files\\Git\\bin\\bash.exe" all accepted
    const lower = shell.toLowerCase();
    if (lower.endsWith("cmd") || lower.endsWith("cmd.exe")) return [shell, "/c", cmd];
    if (lower.includes("powershell") || lower.includes("pwsh")) {
      // ★ PowerShell must mount a PSDrive first inside the sandbox, or every
      //   relative path points at C:\.
      //
      //   The symptom: the process cwd is correct
      //   ([Environment]::CurrentDirectory is fine, and .NET relative paths are
      //   fine too), but PowerShell's **provider location** falls back to C:\
      //   -- so relative paths in cmdlets like Get-ChildItem / Get-Content aim
      //   at the wrong place. Two sets of relative-path semantics in one
      //   process, and extremely hard to diagnose.
      //
      //   The cause: changing directory in PowerShell requires access to the
      //   target's **parent**, while the sandbox grants only the workspace
      //   itself (the ancestor chain cannot be granted -- changing the ACL on
      //   C:\Users\<user> triggers an inheritance recomputation across the
      //   entire profile; measured, ten minutes without returning).
      //
      //   `Set-Location <absolute path>` fails the same way (Access is denied).
      //   What does work, measured, is New-PSDrive: it creates a drive rooted
      //   directly at the workspace and never walks the ancestor chain. Once
      //   mounted, Set-Location hx: succeeds.
      const fix =
        "New-PSDrive -Name hx -PSProvider FileSystem " +
        "-Root ([Environment]::CurrentDirectory) -Scope Global | Out-Null; " +
        "Set-Location hx:; ";
      return [shell, "-NoProfile", "-NonInteractive", "-Command", fix + cmd];
    }
    return [shell, "-c", cmd];
  }
  return isWindows ? ["cmd", "/c", cmd] : ["bash", "-c", cmd];
}

/** The shell's name as shown to the model. Get it wrong and it will keep
 *  sending commands in the other dialect. */
export function shellName(shellOverride?: string): string {
  const shell = shellOverride?.trim();
  if (shell) return shell;
  return isWindows ? "cmd.exe" : "bash";
}

/**
 * Whether path lies inside root (judged on path-component boundaries).
 *
 * ★ Windows has two differences that are not optional; missing either is a
 *   **permission decision going wrong**, not a display problem:
 *     1. the separator may be '/' or '\\', so one directory has two spellings
 *     2. NTFS is case-insensitive by default, so "C:\\Repo" and "c:\\repo" are
 *        the same directory
 *   Compare bytes and a subagent need only respell the root in a different
 *   case for intersect to treat it as a "new root" rather than a subset of the
 *   parent's.
 */
export function pathWithin(path: string, root: string): boolean {
  const norm = (p: string): string => {
    let s = isWindows ? p.replace(/\//g, "\\") : p;
    if (isWindows) s = s.toLowerCase();
    // Drop trailing separators so "C:\\a\\" equals "C:\\a" (except for the
    // roots "C:\\" and "/")
    while (s.length > 1 && (s.endsWith("/") || s.endsWith("\\"))) {
      if (isWindows && s.length === 3 && s[1] === ":") break;
      s = s.slice(0, -1);
    }
    return s;
  };
  const p = norm(path);
  const r = norm(root);
  if (p === r) return true;
  const sep = isWindows ? "\\" : "/";
  return p.startsWith(r.endsWith(sep) ? r : `${r}${sep}`);
}
