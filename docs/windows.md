# Running hx on Windows

*English · [中文](windows.zh-CN.md)*

hx is an agent harness with an OS-level sandbox. Commands the model produces
run inside an **AppContainer plus a Job object**: they cannot read your user
directory, the network is off by default, timeouts can kill them, and
background processes cannot get away. That is not a convention -- the kernel
enforces it.

The Windows implementation is native. Not WSL, not a container, not a
compatibility shim.

---

## Contents

- [Install](#install) · [Configure](#configure) · [Run](#run) · [Interactive mode](#interactive-mode)
- [One-time machine setup: the volume root ACE](#one-time-machine-setup-the-volume-root-ace)
- [Verify it yourself](#verify-it-yourself)
- [Sandbox boundaries](#sandbox-boundaries)
- [When things go wrong](#when-things-go-wrong)
- [Building from source](#building-from-source)

---

## Install

Requirements: **Windows 10 1809 or newer** (AppContainer + ConPTY) and
**Node.js 20+**. The engine is a native binary with no runtime dependencies;
the host is JS, hence Node.

1. Download `hx-windows-x64-<version>.zip` from
   [Releases](https://github.com/lomolomo2/hx/releases)
2. Unpack it somewhere permanent, e.g. `C:\tools\hx`
3. Add that directory to your **user PATH**

Adding to PATH deserves a note of its own. If your user PATH contains
placeholders such as `%NVM_HOME%` or `%USERPROFILE%` (registry type
`REG_EXPAND_SZ`), **do not** use
`[Environment]::SetEnvironmentVariable(...,"User")` -- it writes the value back
as `REG_SZ`, the placeholders become literal text, and nvm's version switching
silently stops working, with symptoms that look nothing like a PATH problem.
Either use the graphical System Settings dialog, or:

```powershell
$k = Get-Item "HKCU:\Environment"
$old = $k.GetValue("Path", "", [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
Set-ItemProperty "HKCU:\Environment" Path ($old.TrimEnd(';') + ";C:\tools\hx") -Type ExpandString
```

Afterwards **open a new terminal**; already-open ones cannot see the change.

Verify the install:

```powershell
hx -Caps
```

Exit code 0 means this machine has a real sandbox.

### Type `hx`, not `.\hx.ps1`

The default Windows execution policy is `Restricted`, so running a `.ps1`
directly is refused:

```
.\hx.ps1 : File ...\hx.ps1 cannot be loaded because running scripts is
disabled on this system.
```

`hx.cmd` starts PowerShell with `-ExecutionPolicy Bypass`, which is exactly why
it exists. **You do not need to change your machine's execution policy to use
hx.** Without PowerShell 7 installed it falls back to `powershell.exe`.

---

## Configure

Any OpenAI-compatible endpoint works: a local llama.cpp / vLLM / DeepSeek /
OpenRouter, and so on.

Precedence is **command line > environment > `~\.hx\config.json`**. Day to day,
the config file is the least trouble:

`C:\Users\<you>\.hx\config.json`

```json
{
  "baseUrl": "https://192.168.1.241/llm/v1",
  "model": "qwen3.8-27b-uncensored",
  "contextWindow": 32768,
  "apiKey": ""
}
```

`contextWindow` **must match the server's `n_ctx`**. Too high and the server
truncates silently (history quietly disappears, and the model starts asking
again about things it has already been told); too low and compaction kicks in
early, throwing information away for nothing. llama.cpp exposes the real value
at `/props`.

### Self-signed certificates

Node's fetch refuses a self-signed endpoint outright with
`DEPTH_ZERO_SELF_SIGNED_CERT`. Put the PEM at:

```
C:\Users\<you>\.hx\certs\llm-<hostname>.pem
```

For example, an endpoint at `https://192.168.1.241/llm/v1` needs
`llm-192.168.1.241.pem`. The launcher finds it automatically and trusts
**only** it, via `NODE_EXTRA_CA_CERTS`.

Do not use `NODE_TLS_REJECT_UNAUTHORIZED=0` -- that switches off TLS
verification for the whole process, leaving it defenceless against any man in
the middle, a price far beyond "connect to this one machine".

---

## Run

```powershell
cd C:\path\to\your\repo

hx                                   # interactive mode
hx "fix the failing tests"           # one-shot: run to completion and exit
hx -Net allow -Approval auto -MaxSteps 40 "..."
```

The current directory is the workspace (use `-Root` to point elsewhere). **The
sandbox lets it touch only that one tree.**

Common flags:

| Flag | Default | Meaning |
|---|---|---|
| `-Root` | the current directory | The workspace |
| `-Net` | `deny` | `deny` / `allow`. deny is kernel-enforced and blocks both TCP and UDP |
| `-Sandbox` | `workspace-write` | `read-only` / `workspace-write` / `danger-full-access` |
| `-Approval` | `cautious` | `auto` / `cautious` / `strict` |
| `-MaxSteps` | 20 | Maximum steps per turn |
| `-ReadPath` | — | An extra read-only grant; repeatable. Needed when a toolchain lives elsewhere |
| `-BaseUrl` `-Model` `-ApiKey` `-ContextWindow` | see config.json | Temporary overrides |
| `-CaCert` | auto-discovered | Specify the PEM |

`-Approval auto` means no questions and everything allowed -- use it only when
you trust the workspace. The default `cautious` stops and asks before writing
files or running suspicious commands.

---

## Interactive mode

With no task and stdin attached to a terminal it enters interactive mode. **One
session and one history** persist across turns; nothing starts over.

```
› first, see how this repo organizes its tests
› now fix the failing parser cases
› /sandbox
```

| Command | Effect |
|---|---|
| `/help` | Help |
| `/tools` | The tools the model can **actually** see this turn (policy hiding shows up here) |
| `/sandbox` | This session's sandbox reality: enforced / backend / warnings |
| `/exit` `/quit` | Quit (Ctrl+D, or Ctrl+C at an empty prompt, do the same) |

**Ctrl+C mid-run** interrupts the current turn at a *phase boundary* rather
than cutting in immediately -- severing a tool halfway leaves side effects the
engine knows nothing about. Commands already started are wound up by the
engine's timeout and `exec.kill`.

Feeding input through a pipe (CI, scripts) does not enter interactive mode and
prints usage instead -- there it would read EOF immediately and look like it
did nothing at all.

---

## One-time machine setup: the volume root ACE

**Symptom**: `dir` / `Get-ChildItem` fail everywhere inside the sandbox, while
reading files, writing files and running programs all work. The model ends up
going in circles over "where are my files".

**Cause**: an AppContainer cannot traverse the volume root (`C:\`) by default,
and cmd's `dir` queries volume information.

**Fix** (once per drive you use as a workspace; **administrator rights
required**):

```powershell
icacls C:\ /grant "*S-1-15-2-1:(S,X,RA)"
```

`(S,X,RA)` = synchronize + traverse + read-attributes. It does **not** grant
`FILE_READ_DATA` (on a directory, that is "list contents") and does not
inherit -- so `dir C:\` inside the sandbox is **still refused**; it merely
stops blocking `dir` in your own workspace. The shape matches the capability
ACE Windows itself ships on `C:\`.

`SYNCHRONIZE` must not be omitted: with only `(X,RA)`, `dir` still says "Access
is denied".

To undo:

```powershell
icacls C:\ /remove:g "*S-1-15-2-1"
```

**The engine only checks; it never changes this for you.** When the grant is
missing it reports so honestly in `session.open`'s warnings, along with the
exact command. This is deliberate: measured, a single `icacls C:\ /grant` takes
**16 seconds** (the volume root's children take part in inheritance
propagation), and a session would grant at the start and revoke at the end,
adding 30-odd seconds to every `session.open`. Beyond the cost, it is a
persistent modification of the user's disk, and that should not be done by
something that happens on every single run.

Data drives (`D:\`, `H:\`, ...) usually have no AppContainer grant at all and
need the same one-time treatment.

---

## Verify it yourself

Do not take the documentation's word for it. Run these.

### 1. Isolation capability self-test

```powershell
hx -Caps
```

```json
{"platform":"windows","kernel":"10.0.26200",
 "filesystem":{"available":true,"backend":"appcontainer"},
 "network":{"available":true,"backend":"appcontainer-capabilities","covers_udp":true},
 "limits":{"available":true,"backend":"job-object","nested_jobs":true,
           "max_processes":true,"max_memory":true,
           "max_file_bytes":false,"max_open_files":false},
 "pty":{"available":true,"backend":"conpty"}}
```

Note the two `false` values under `limits` -- Job objects have no counterpart
for `RLIMIT_FSIZE` / `RLIMIT_NOFILE`, and the engine does not pretend they were
applied. **This report is probed, not hardcoded**: it really does create an
AppContainer profile and delete it, and really does set a Job limit and read it
back.

### 2. The sandbox in practice (needs the repository; not in the release package)

```powershell
pwsh engine\tests\windows\smoke.ps1
```

24 checks, verifying both that what should be refused is refused and that what
should pass gets through. The latter is the point: this suite was once 18/18
green while `dir` failed **everywhere** inside the sandbox, because at the time
it only tested `echo`, `type` and writing files. A security test inherently
cannot detect that the permitted was blocked.

### 3. Run a complete task offline

```powershell
pwsh try-hx.ps1
```

It starts a fake OpenAI-compatible endpoint that drives a multi-step task
(read -> modify -> run tests -> verify). Only the decision of what to do next
is replaced; prompts, policy, approval and engine calls all take the real path.

---

## Sandbox boundaries

### What it stops

| | Mechanism |
|---|---|
| `%USERPROFILE%` is unreadable (`.ssh`, browser passwords, tokens) | AppContainer: the profile never granted the package SID |
| Writing outside the workspace | The same; only roots carry ACEs |
| The network is cut off, **TCP and UDP alike** | No `internetClient` capability; WFP blocks in the kernel |
| Process bombs | Job `ActiveProcessLimit` = 512 |
| Memory | Job `JobMemoryLimit` = 4 GiB |
| Background processes cannot get away | `TerminateJobObject`, which is atomic |
| Even a `kill -9`ed engine leaves no orphans | `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` |
| Junction / symlink escapes | Open a handle, then ask the kernel for the final path (`GetFinalPathNameByHandle`) |

The network row is where Windows is stronger than Linux: one capability covers
TCP and UDP with no DNS blind spot (Linux's Landlock covers TCP only and needs
a second seccomp layer to close the gap).

### What it does not stop, and the honest gaps

- **No counterpart for `RLIMIT_FSIZE` / `RLIMIT_NOFILE`.** Job objects have
  neither. Filling the disk and exhausting handles **are not stopped**, and the
  two `false` values in `--self-test` are saying exactly that.
- **`exec.kill`'s TERM / INT return `killed:false`.** Windows has no signals,
  and the engine does not quietly escalate to a hard kill -- that would bypass
  the target's own cleanup logic. Use KILL when you really mean to kill.
- **Under `danger-full-access` there is no kernel enforcement**, leaving only
  path validation.
- **The policy layer (what should be done) is JavaScript and can be bypassed.**
  It and the sandbox layer (what can be done) are two different things and do
  not substitute for each other: policy alone is one line of JS standing
  between an attacker and the system; a sandbox alone lets the agent wreak
  arbitrary havoc within what it was granted.
- **ACEs on roots are a real modification of the disk**, revoked on a clean
  exit; after a `kill -9` the next start cleans the residue up itself (both the
  profile and the ACEs).
- **The sandbox leaves an `AppData\` directory in the workspace.** Because it
  points `USERPROFILE` at the workspace root (so `$HOME` is readable and tools
  do not scribble in your real user directory), and Windows places an
  AppContainer's own redirected writes under
  `%USERPROFILE%\AppData\Local\Packages\<container>\AC\` -- so they land inside
  the workspace. The content is small (typically PowerShell's startup cache)
  and never escapes the workspace, but it **is not cleaned up automatically**
  and accumulates per session. Add `AppData/` to your `.gitignore`.

---

## When things go wrong

### `running scripts is disabled on this system`

You typed `.\hx.ps1`. Type `hx` (or `.\hx.cmd`). See
[that section](#type-hx-not-hxps1) above.

### `dir` reports Access is denied inside the sandbox

The volume root ACE has not been applied. See
[One-time machine setup](#one-time-machine-setup-the-volume-root-ace). The
engine has already given the exact command in `session.open`'s warnings.

### The model keeps sending POSIX commands (`ls`, `cat`, `grep`) and every one fails

The default shell is `cmd.exe`, and the tool description tells the model so.
With Git Bash installed you can set
`$env:HX_SHELL = "C:\Program Files\Git\bin\bash.exe"`.
**Change it and the tool description changes with it** -- call it bash, run cmd
underneath, and tell the model nothing, and it will send POSIX commands
throughout and fail on every one.

### Relative paths in PowerShell point at `C:\`

Already handled, but the symptom is worth knowing: the process cwd is correct
(`[Environment]::CurrentDirectory` is fine, and .NET relative paths are fine
too), but PowerShell's **provider location** falls back to `C:\`, so relative
paths in cmdlets aim at the wrong place -- two sets of relative-path semantics
in one process. The cause is that `Set-Location` needs access to the target's
**parent**, while the sandbox grants only the workspace itself. The fix is
`New-PSDrive`, which creates a drive rooted directly at the workspace and never
walks the ancestor chain. `hx` prepends that automatically.

### Some program will not start inside the sandbox

"What is installed" and "what works inside the sandbox" are two different
things. The test is whether the target directory grants **ALL APPLICATION
PACKAGES**:

```powershell
icacls "C:\Program Files\nodejs"
```

Measured: `C:\Program Files\Git` has it (inherited), while `C:\Program
Files\nodejs` **does not** -- the Node installer breaks ACL inheritance.
Anything under `%SystemRoot%\System32` (`cmd`, `powershell`) always works.

Fixing the ACL requires administrator rights; alternatively, add that directory
explicitly with `-ReadPath`.

### A toolchain installed under `%USERPROFILE%` will not run

nvm / rustup / conda and the like. This **is not a bug but the design working
correctly** -- the user directory is unreadable by default. To use it, grant it
explicitly:

```powershell
hx -ReadPath "$env:USERPROFILE\.cargo" "..."
```

### The model runs out of steps with no progress

First check whether `contextWindow` matches the server's `n_ctx`. When they
disagree the server truncates history silently and the model forgets what it
just did.

### The session log

Every run has an append-only JSONL log at
`%USERPROFILE%\.hx\sessions\<year>\<month>\<day>\rollout-*.jsonl`.

**The first record nails down whether that run had a real sandbox**:

```powershell
$r = Get-ChildItem "$env:USERPROFILE\.hx\sessions" -Recurse -Filter "rollout-*.jsonl" |
     Sort-Object LastWriteTime | Select-Object -Last 1
(Get-Content $r.FullName -TotalCount 1 | ConvertFrom-Json).payload.effective
```

---

## Building from source

Requires **VS2019 16.11+** (for `/std:c++20`) and Node 20+.

```powershell
git clone https://github.com/lomolomo2/hx
cd hx

pwsh engine\build-win.ps1        # finds VS and uses its bundled cmake + Ninja
cd host; npm install; cd ..

.\hx.cmd -Caps
```

`hx.ps1` recognises for itself whether it is in a working tree or a release
package: in a working tree it runs the TypeScript sources through tsx (so edits
take effect immediately), and in a package it runs the prebuilt bundle.

Run the full regression suites:

```powershell
pwsh engine\tests\windows\smoke.ps1     # engine, 24 checks
pwsh host\test\all.ps1                  # host: typecheck / arch / e2e / compaction / approval / subagents / server
```

Build a package yourself:

```powershell
pwsh scripts\package-win.ps1 -Version v0.2.0
```

The packaging script runs `hxd --self-test` before compressing and refuses to
package on a non-zero exit -- shipping a binary that cannot pass its own
self-test pushes the burden of proving "there is a sandbox" onto the user.

---

## How this maps to Linux

The mechanisms differ; **the properties are the same**: all allow-lists, all
enforced in the kernel, none relying on blacklists.

| The problem to solve | Linux | Windows |
|---|---|---|
| Files granted by allow-list | Landlock ruleset | AppContainer SID + directory ACEs |
| Cutting off the network | Landlock(TCP) + seccomp(AF_INET) | No `internetClient` (one layer covers TCP+UDP) |
| Process count limit | `RLIMIT_NPROC` (by UID, counts threads) | Job `ActiveProcessLimit` (by Job, counts processes) |
| Memory limit | None (cgroup not wired into spawn) | Job `JobMemoryLimit` |
| Collecting a whole process tree | setsid + kill(-pgid) + repeated sweeping | `TerminateJobObject` (atomic) |
| Pseudo-terminal | forkpty | ConPTY |
| Event loop | epoll | IOCP + overlapped named pipes |

More detail is in the repository's [README](../README.md) and in the comments
of the individual source files.
