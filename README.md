# hx

*English · [中文](README.zh-CN.md)*

An agent harness with an OS-level sandbox. The engine is a native binary; the
host is TypeScript.

```
LLM (untrusted, generates intent at high volume)
  ↓ tool calls -- the only entry point
hx-host  (TypeScript)   decides what SHOULD be done: context assembly, prompts, policy, orchestration
  ↓ hxp/0 -- JSONL over stdio, the only entry point
hxd      (C++)          decides what CAN be done: Landlock / seccomp / rlimit / exec / patches / logs
  ↓
the real world
```

**The core constraint**: `hx-host` may not `import node:fs` or
`node:child_process` (enforced by `test/arch.sh`). Every side effect must pass
through the engine. That is what makes "the host is userland relative to the
engine" an architectural fact rather than a slogan.

This design did not come out of nowhere. Four threads run through the code
comments; here are the conclusions up front:

- **Survey of the ground**: after comparing codex (deep: sandbox / approval /
  patches), pi (a white-box runtime library), opencode (harness as a service)
  and DeerFlow 2.0 (lead agent + subagents), the choice was a two-process
  privilege separation: a native engine plus a TypeScript host.
- **The privilege boundary**: the whole security model is mapped against
  Windows's user/kernel divide -- a single syscall entry point, parameter
  capture, HANDLEs as opaque IDs, access masks, interceptable IRPs, IRQL phase
  constraints, UIPI integrity levels. Each correspondence is documented in the
  comments of the relevant source file.
- **The scheduling model**: the turn loop is mapped against the Win32 message
  loop (a stateless WndProc = a stateless model; WM_PAINT coalescing = context
  compaction).
- **Order of implementation**: protocol before implementation; a minimal closed
  loop before engineering properties; and the foundations (two streams, an
  append-only source of truth, complete state snapshots) fixed on day one.

---

## Quick start

### Build the engine

```bash
cd engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8
./build/hxd --self-test          # prints this machine's isolation capabilities; exit code 0 = a real sandbox
```

Linux and Windows. `--self-test`'s exit code means the same thing on both
platforms: **0 = this machine has a real sandbox, 2 = it does not**. When it
does not, the engine reports "no filesystem isolation" honestly rather than
pretending to be safe.

- **Linux** needs Landlock enabled in the kernel (`landlock` should appear in
  `cat /sys/kernel/security/lsm`).
- **Windows** needs Windows 10 1809 or newer (AppContainer + ConPTY).
  Build with MSVC (VS2019 16.11 or later; `/std:c++20` required):

  ```powershell
  cd engine
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build build
  .\build\hxd.exe --self-test
  ```

  Typical output (this machine has the full set of isolation):

  ```json
  {"platform":"windows","kernel":"10.0.26200",
   "filesystem":{"available":true,"backend":"appcontainer"},
   "network":{"available":true,"backend":"appcontainer-capabilities","covers_udp":true},
   "limits":{"available":true,"backend":"job-object","nested_jobs":true,
             "max_processes":true,"max_memory":true,
             "max_file_bytes":false,"max_open_files":false},
   "pty":{"available":true,"backend":"conpty"}}
  ```

  Note the two `false` values under `limits`: Job objects have no counterpart
  for `RLIMIT_FSIZE` / `RLIMIT_NOFILE`, and the engine does not pretend they
  were applied. See "How the two platforms correspond".

  **One-time machine setup (whether to do it is your call)**: an AppContainer
  cannot read the volume root by default, and `dir` / `Get-ChildItem` query
  volume information -- without this, **no directory can be listed** inside the
  sandbox (reading files, writing files and running programs are unaffected).
  The engine reports this honestly in `session.open`'s warnings along with the
  exact command. Do it once per drive you want to use as a workspace;
  **administrator rights required**:

  ```powershell
  icacls C:\ /grant "*S-1-15-2-1:(S,X,RA)"
  ```

  It grants synchronize + traverse + read-attributes only, **not**
  list-contents, and does not inherit -- so `dir C:\` is still refused. The
  shape matches the capability ACE Windows itself ships on `C:\`.
  To undo: `icacls C:\ /remove:g "*S-1-15-2-1"`.

### Run a task

```bash
cd host && npm install

# Any OpenAI-compatible endpoint: a local llama.cpp / DeepSeek / OpenRouter / vLLM ...
export HX_BASE_URL=http://127.0.0.1:8080/v1
export HX_MODEL=qwen3.8-27b-uncensored
export HX_CONTEXT_WINDOW=16384          # must match the server's n_ctx

npx tsx src/cli.ts --root /path/to/repo "fix the failing tests"   # one-shot
npx tsx src/cli.ts --root /path/to/repo                           # interactive
```

With no task and stdin attached to a terminal it enters **interactive mode**:
one session and one history across repeated turns, with `/help` `/tools`
`/sandbox` `/exit`, and Ctrl+C mid-run interrupting the current turn at a phase
boundary. It does not start without a terminal (a pipe, CI) -- there it would
read EOF immediately and look like it did nothing at all.

### Windows: use it as a single command

> The complete Windows guide (install, configure, verify, troubleshoot,
> sandbox boundaries) is in **[docs/windows.md](docs/windows.md)**. If you
> would rather not build from source, [Releases](https://github.com/lomolomo2/hx/releases)
> has a prebuilt package: unpack it, add it to PATH, and it works (the engine
> is a native binary; the host needs Node 20+).

`hx.ps1` / `hx.cmd` in the repository root are a launcher. Once that directory
is on `PATH`:

```powershell
cd C:\path\to\your\repo
hx                                  # the current directory is the workspace; interactive
hx "fix the failing tests"          # one-shot
hx -Net allow -Approval auto -MaxSteps 40 "..."
```

**Type `hx` (which runs `hx.cmd`), not `.\hx.ps1`.** The default Windows
execution policy is `Restricted`, so running a `.ps1` directly is refused
(`running scripts is disabled on this system`). `hx.cmd` starts pwsh with
`-ExecutionPolicy Bypass`, which is exactly why it exists -- so you never have
to change your machine's execution policy just to use hx. Without PowerShell 7
it falls back to `powershell.exe`.

Endpoint configuration precedence: command line > environment >
`~\.hx\config.json`:

```json
{ "baseUrl": "https://192.168.1.241/llm/v1", "model": "qwen3.8-27b-uncensored", "contextWindow": 32768 }
```

For a self-signed certificate, put the PEM at `~\.hx\certs\llm-<hostname>.pem`;
the launcher trusts **only** it via `NODE_EXTRA_CA_CERTS` rather than switching
off TLS verification for the whole process.

### Serve it

```bash
npx tsx src/server/main.ts --port 4100

curl -X POST localhost:4100/session -H 'content-type: application/json' \
  -d '{"roots":["/path/to/repo"],"sandbox":"workspace-write","net":"deny"}'
curl -N localhost:4100/session/s1/event                       # the SSE event stream
curl -X POST localhost:4100/session/s1/prompt -H 'content-type: application/json' \
  -d '{"text":"your task"}'
```

---

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `HX_BASE_URL` | — | An OpenAI-compatible endpoint; required |
| `HX_MODEL` | — | Model name; required |
| `HX_API_KEY` | empty | Usually unnecessary for local models |
| `HX_MAX_TOKENS` | 4096 | Set too low and a reasoning model returns empty `content` |
| `HX_CONTEXT_WINDOW` | 32768 | **Must match the server's `n_ctx`**: too high and the server truncates silently, too low and compaction kicks in early |
| `HX_RESERVE_TOKENS` | 4096 | Headroom left for the reply |
| `HX_KEEP_RECENT_TOKENS` | 6144 | How many trailing tokens stay verbatim |
| `HX_APPROVAL` | cautious | `auto` / `cautious` / `strict` |
| `HX_ENGINE` | `../engine/build/hxd` (`hxd.exe` on Windows) | Engine path |
| `HX_SHELL` | `bash` on Linux, `cmd` on Windows | Which shell the `bash` tool uses. **Change it and the dialect name shown to the model changes too** -- call it bash, run cmd underneath, and tell the model nothing, and it will send POSIX commands throughout and fail on every one |
| `HX_SHOW_REASONING` | — | Set it to display a reasoning model's thinking summary |
| `HX_LOG_PROMPTS` | — | Set to `1` to also record the exact input handed to the model each step as `model_call` records. Off by default because step N's prompt contains the whole history up to N, so it costs O(n²) -- measured, a 7-step run's rollout went from 6 KB to 50 KB. Read them back with `node scripts/transcript.mjs --prompts` |

CLI flags: `--root` `--sandbox` `--net` `--approval` `--max-steps` `--engine`.

---

## The security model

**Two layers, orthogonal, neither substituting for the other.**

| | Governs | Can it be bypassed | What it stops |
|---|---|---|---|
| Policy layer (host) | What should be done (intent) | Yes -- it is only JavaScript | Things that are technically possible but should not happen |
| Sandbox layer (kernel) | What can be done (capability) | No | Things the model wants to do after being injected |

Policy without a sandbox = one line of JavaScript standing between an attacker
and the system.
A sandbox without policy = the agent can wreak arbitrary havoc within what it
was granted.

### What the sandbox layer does

Described below for Linux. Each Windows counterpart is in "How the two
platforms correspond"; the mechanisms differ but **the properties are the
same**: all allow-lists, all enforced in the kernel, none relying on
blacklists.

- **Landlock**: files are granted by allow-list. The default system read-only
  paths **do not include `$HOME`** -- that alone is the entire reason
  `cat ~/.ssh/id_rsa` returns EACCES, with no blacklist involved. Symlink
  escapes are blocked automatically (Landlock judges on the resolved path).
  The Windows counterpart is AppContainer: the process token carries a
  low-privilege package SID, and only objects whose DACL explicitly grants it
  are allowed. The user profile does not grant it by default, so
  `type %USERPROFILE%\.ssh\id_rsa` is a flat ACCESS_DENIED -- the same
  mechanism. Symlink and junction escapes are blocked by `path_guard` opening a
  handle and asking the kernel for the final path
  (`GetFinalPathNameByHandle`, the equivalent of `realpath`).
- **seccomp-bpf**: blocks `socket()` for `AF_INET`/`AF_INET6` (Landlock covers
  TCP only, with UDP/DNS its blind spot), plus syscalls irrelevant to a build
  task such as `ptrace`/`mount`/`keyctl`/`bpf`. It returns EPERM rather than
  KILL -- a tool that can report an error is what makes the model try another
  route.
- **rlimit**: `RLIMIT_NPROC` = **current task count + 512** (an absolute value
  must never be hardcoded -- it counts **threads, not processes**; on this
  machine 104 processes correspond to 667 threads, and hardcoding 256 once made
  every fork inside the sandbox fail), `RLIMIT_FSIZE` 2 GiB, `RLIMIT_NOFILE`
  4096.
- **The process group is ownership**: when a cell's direct child exits, if the
  process group still has members the engine freezes (SIGSTOP) and sweeps
  (SIGKILL) until it is empty. Without this, any `cmd &` leaves a daemon behind
  on the system.
- **Order of application**:
  `fork -> chdir -> landlock_restrict_self -> rlimit -> seccomp -> execve`.
  Landlock cannot revoke already-open fds, and seccomp applied too early blocks
  the steps that follow -- **the wrong order means it never happened**.
- **If the sandbox cannot be applied, do not execute** (`_exit(126)`), and
  never degrade to running unprotected.

### The honest boundaries

- **`RLIMIT_NPROC` counts by real UID, and counts threads.** The limit can only
  be set relative to current usage (current + 512). A fork bomb is stopped and
  the machine survives, but the same user's other processes may also fail to
  fork during the attack. **This is mitigation, not isolation.**
- **cgroup v2 is probed but unusable on this machine.** `--self-test` actually
  walks "enable the controllers -> create a child cgroup -> write
  pids.max/memory.max -> read back and verify", and reports honestly:

  ```json
  "cgroup2": {"available": true, "usable": false,
    "reason": "enable subtree_control: ...: Device or resource busy"}
  ```

  `available` only says the filesystem is there; `usable` says the limits can
  be applied. In an ordinary user session hxd shares a scope with the shell, so
  writing `cgroup.subtree_control` gives `EBUSY` (cgroup v2's "no internal
  processes" rule). Put the engine in its own scope
  (`systemd-run --user --scope -p Delegate=yes`) and it works -- that is a
  deployment choice, and not the engine's to make unilaterally. The probe
  itself has no side effects: it restores only the controllers it newly
  enabled and leaves the user's own alone.
  **Note that `Cgroup2Session` is not yet wired into the spawn path**; only the
  probe and the session primitives exist today.
- **Under `danger-full-access` there is no kernel enforcement**, leaving only
  `path_guard`'s soft validation.
- Linux and Windows. macOS Seatbelt has an interface seam
  (`sandbox/confine.hpp`) but is not implemented.

---

## How the two platforms correspond

The security model was designed against Windows's user/kernel divide to begin
with (see "The privilege boundary" above), so moving to Windows is not
"finding an approximation that will do" -- it is returning to the prototype.
The details of each row live in the comments of the corresponding source file.

| The problem to solve | Linux | Windows | Which is stronger |
|---|---|---|---|
| Files granted by allow-list | Landlock ruleset | AppContainer SID + directory ACEs | Tie |
| `$HOME` unreadable by default | Not added to the ruleset | The user profile does not grant the package SID | Tie |
| Cutting off the network | Landlock(TCP) **+** seccomp(AF_INET, covering the UDP blind spot) | No `internetClient` capability; WFP blocks in the kernel | **Windows**: one layer covers TCP+UDP, with no blind spot |
| Process count limit | `RLIMIT_NPROC` (by UID, counts threads) | Job `ActiveProcessLimit` (by Job, counts processes) | **Windows**: an absolute value is fine and does not affect the user's other processes |
| Memory limit | None (cgroup not wired into the spawn path) | Job `JobMemoryLimit` | **Windows** |
| Per-file size / fd count limit | `RLIMIT_FSIZE` / `RLIMIT_NOFILE` | **No counterpart** | **Linux** |
| The process group is ownership | `setsid` + `kill(-pgid)` + repeated sweeping | Job objects; `TerminateJobObject` terminates atomically | **Windows**: there is no "racing against fork" at all |
| After the engine itself is `kill -9`ed | Descendants are reparented to init and live on | `KILL_ON_JOB_CLOSE`; the kernel collects them too | **Windows** |
| Interactive sessions | `forkpty` | ConPTY (`CreatePseudoConsole`) | Tie |
| Event loop | epoll | IOCP + overlapped named pipes | Tie |
| When privileges drop | The child calls `landlock_restrict_self` after fork; **the wrong order means it never happened** | The token is fixed at `CreateProcess` time, leaving the child no window | **Windows**: this class of ordering bug does not exist |

### The honest boundaries on Windows

- **`RLIMIT_FSIZE` / `RLIMIT_NOFILE` have no counterpart.** Job objects limit
  neither per-file size nor handle count. `--self-test` reports both as
  `false`, so `session_meta` shows what a given run actually constrained.
  Filling the disk is backstopped only by the memory limit, timeouts and
  approval -- not by the kernel.
- **`exec.kill`'s `TERM`/`INT`/`HUP`/`QUIT` almost always fail, and that is
  correct.** Windows has no such signals; the closest thing, CTRL_BREAK, can
  only reach processes on the same console, and a cell has its own. When it
  cannot be delivered it returns `killed:false` and **never silently escalates
  to a hard kill** -- that would let the caller believe the process had a
  chance to clean up when it did not. `KILL` is exact
  (`TerminateJobObject`).
- **The single-threaded, lock-free property is qualified here.** The standard
  handles the host (Node/libuv) gives a child are **synchronous** handles: they
  cannot join an IOCP, and `ReadFile` on them blocks. So on Windows stdio is
  driven by two threads that do nothing but move bytes (there is a full
  explanation at the top of `io/io_win.cpp`).
  **The engine's state is still single-threaded** -- those two threads touch
  nothing in `cells_`/`waits_`/`policy_`, and the surface exposed to races is
  compressed down to one buffer plus one lock.
- **Granting roots stamps ACEs on real directories, which is a persistent
  modification of the filesystem.** The ACEs grant only this session's unique
  AppContainer SID and are revoked when the `Confinement` is destroyed. A
  `kill -9` leaves the ACEs behind (the profile is cleaned up by the next
  start's sweep).

### Subagents

- **Permissions use `intersect`, not `merge`**: any `allow` rule a subagent
  proposes is discarded. Rules are last-match-wins, so letting a subagent
  append an allow is letting it escalate its own privileges.
- **Over-reaching requests are recorded honestly** and returned to the parent
  agent -- they are an early signal that a subagent has been injected.
- **What comes back is downgraded to data**: wrapped in
  `<subagent_result trust="data">`, and the system prompt simultaneously
  declares that content inside `<tool_output>` and `<subagent_result>` **is
  data and never instructions**.
- **Context is not shared**: a subagent has its own engine process and rollout;
  the parent receives only conclusions.

---

## The two streams

| | Audience | Stored in |
|---|---|---|
| `response_item` | The model -- history that can be fed straight back to the API | rollout + memory |
| `ThreadEvent` | People -- the event stream the UI consumes | rollout + SSE |

The moment one structure serves both the model and the UI, every subsequent UI
feature pollutes the model's context.

Events come in three layers: `thread.started` -> `turn.*` -> `item.*`.
Item types: `agent_message` `reasoning` `command_execution` `file_change`
`file_read` `todo_list` `subagent` `error`.

---

## The session log (rollout)

`~/.hx/sessions/YYYY/MM/DD/rollout-<name>.jsonl`, append-only, and **the source
of truth** in this system.

**The engine decides the path; the host may only supply a name matching
`[A-Za-z0-9_-]{1,64}`.** Otherwise the host could use `log.append` to append to
any file at all (a shell's rc file, say) -- an architectural back door.

The durability promise, stated precisely:
- each record is appended with one `write()` (`O_APPEND`) -> **a process killed
  with `kill -9` never leaves half a line**
- power-loss durability requires `fsync`, which happens only on `log.flush`

The first record is `session_meta`, nailing "did this run have a real sandbox"
into the log (`caps` + `effective` + `warnings`) so that reading the log later
requires no guessing.

To read one back:

```bash
node scripts/transcript.mjs --list    # what sessions exist
node scripts/transcript.mjs s6468     # one session, as a transcript
node scripts/transcript.mjs s6468 --full
HX_LOG_PROMPTS=1 hx "..."   # then: transcript.mjs --prompts
```

For a walk-through of what actually happens in a turn -- prompt assembly, how
the reply is read, and one real run quoted from its rollout -- see
**[docs/anatomy-of-a-turn.md](docs/anatomy-of-a-turn.md)**.

It pairs each tool call with its result and renders the sandbox report,
approvals, subagent grants and compactions. Note what a rollout does *not*
hold: the exact text sent to the model. Each step's prompt is assembled fresh
by `buildPrompt()` and never written to disk; what is recorded is the
conversation.

Context compaction writes a `compacted` record carrying **the original text it
replaced** (`replacement_history`), the summary, and token counts before and
after. **Compaction is an auditable record, not a quiet rewrite of history** --
otherwise, while debugging, you could never work out what the model saw at the
time.

---

## Context management

- **Self-calibrating token estimation**: the real `usage` from each call is
  used to derive the characters-per-token ratio backwards. That ratio differs a
  great deal between mixed-script prose, code and JSON, so a hardcoded constant
  is bound to be wrong.
- **Two compaction invariants**: never split `assistant(tool_calls)` from its
  `tool` results (split them and the server rejects the request outright); and
  always keep the final assistant+tool group (otherwise "a large file just read
  is immediately compacted away", the model believes it never read it, reads it
  again, and loops forever).
- **The summary input must include the original task.** With it missing, the
  model wrote *"The original task statement isn't in the visible history"* into
  its summary, then dropped the goal and started exploring again.
- **Middle truncation**: command output keeps the head (what ran, the first
  error) and the tail (the final state, the conclusion, the stack trace), and
  cuts the middle.

---

## Tests

```bash
cd host && npm test          # Linux: eleven suites
```

```powershell
cd host; pwsh test\all.ps1   # Windows: seven suites
```

| Suite | What it verifies |
|---|---|
| typecheck | TS strict fully enabled |
| arch.sh | The host must not touch fs / start processes (self-checking: a deliberate violation is caught) |
| escape/run.sh | 22 checks: escape blocking + **usability (what should pass must get through)**, `xfail=0` |
| durability/kill9.py | `kill -9` mid-write; 100,000 lines with zero corruption |
| limits/forkbomb.py | A fork bomb stays bounded and is cleaned up; `cmd &`'s background process does not leak |
| pty/interactive.py | REPL interaction + **the sandbox is equally enforced on the separate pty path** |
| e2e.ts | A multi-step task: plan -> tests fail -> read -> patch -> tests pass |
| compaction.ts | Compaction triggers, leaves a trace, and never splits a tool pair |
| approval.ts | The rule engine + ask suspend/resume + allow_always |
| subagent.ts | Monotonically non-increasing permissions, context isolation, content downgrade |
| server.ts | The full HTTP/SSE flow + approval over HTTP |

On Windows the four engine suites `escape` / `kill9` / `pty` / `forkbomb` have
no direct counterpart (they probe the specific behaviour of Landlock / seccomp
/ RLIMIT), and are replaced by `engine/tests/windows/smoke.ps1`: **24 checks
verifying the same set of properties** -- what should pass gets through, what
should be refused is refused, the process tree does not leak, timeouts kill,
pty works, and residue from a hard kill is cleaned up by the next start -- just
by asking AppContainer and Job objects instead. One of them cannot be verified
on Linux at all: whether `net=deny` blocks **UDP/DNS** (Landlock's blind spot,
which Linux patches with seccomp and Windows covers natively).

**Two lessons**:

1. A security test verifies that the forbidden was refused; it cannot detect
   that the permitted was blocked. Granting `/dev/null` with the wrong access
   bits, and hardcoding `RLIMIT_NPROC` to 256 -- both times the escape suite was
   entirely green while the actual work was entirely broken. That is why the
   escape suite has a dedicated "usability: what should pass must get through"
   section.
2. **The test path must equal the production path.** `--sandbox-exec` once
   failed to set `TMPDIR` and did not kill the process group, so tests passed
   in an environment that does not exist in a real session. This trap has been
   hit three times in this project.

---

## Layout

```
proto/hxp-v0.md            the protocol spec (written before the implementation;
                           when they disagree, the implementation is what changes)
engine/
  src/platform/            NowMs, path shape, file primitives (one implementation per platform)
  src/io/                  Reactor + non-blocking streams (Linux epoll / Windows IOCP)
  src/sandbox/             caps probing, policy, confine (the platform seam)
      landlock, seccomp, cgroup           -- Linux
      confine_win (AppContainer + ACEs)   -- Windows
  src/exec/                spawn, pty, cell, env, rlimit, proc (process-tree ownership)
  src/fs/                  path_guard (the only place a string becomes a path), apply_patch
  src/log/                 rollout
  src/engine.cpp           the event loop and op dispatch (single-threaded reactor; engine state is lock- and race-free)
  tests/                   escape, durability, pty, limits  -- Linux
      windows/smoke.ps1                                     -- Windows
host/
  src/platform.ts          host-side platform differences (engine path, shell, path comparison)
  src/protocol/            ThreadEvent / Item / hxp types
  src/engine/client.ts     the only file allowed to spawn the engine
  src/tools/               bash read apply_patch glob grep task todo
  src/context/             budget, compact, truncate
  src/policy/              rules, presets, approval, intersect
  src/loop/turn.ts         the four-phase state machine
  src/server/              Hono + SSE
  test/all.sh, all.ps1     each platform's full regression suite
hx.ps1, hx.cmd             the Windows launcher: add to PATH and type `hx` in any repo
try-hx.ps1                 the Windows try-it entry point (-Caps / -Sandbox / -Repl / an offline task)
scripts/transcript.mjs     read a rollout back as a readable transcript (node scripts/transcript.mjs --list)
docs/anatomy-of-a-turn.md  what happens in one turn, with a real run quoted end to end
```

**The principle for splitting by platform: select whole files, never scatter
`#ifdef` inside them.** Scattered `#ifdef`s let the two paths drift out of sight
of each other, so one gets changed and the other forgotten; splitting by file at
least forces the function signatures on both sides to line up, with the
compiler watching. The selection lives in `engine/CMakeLists.txt`.

`turn.ts`'s four phases are explicit state:
`assembling -> streaming -> executing -> settling`. "Context compaction fires
while a tool is mid-execution" is a real class of bug, and the same kind of
error as "touching paged memory at `DISPATCH_LEVEL`" -- built as explicit
state, `PhaseError` throws on the spot when it is written wrong.

---

## Known traps (all of them hit)

- **Toolchains that nvm / rustup / conda install into `$HOME` cannot run in the
  sandbox**, because `$HOME` is unreadable by default. This is not a bug but the
  design working correctly; add the path to roots explicitly.
- **`HX_CONTEXT_WINDOW` must match the server's `n_ctx`.** llama.cpp exposes it
  at `/props`.
- **Python's `.pyc` cache will lie to you**: if a change keeps the byte count
  identical and lands within the same second, the `(mtime seconds, size)` check
  judges the cache valid -- so the agent fixed it correctly and the test still
  reports failure. Test with `python3 -B`.
- **`bash -lc` reads `~/.profile`**, and `$HOME` is unreadable, so every
  command carries a line of noise. Use `bash -c`.
- **`RLIMIT_NPROC` counts threads, not processes.** An absolute value must
  never be hardcoded: on this machine 104 processes correspond to 667 threads,
  and hardcoding 256 makes every `fork` inside the sandbox fail.
- **A cell must own its process group.** The top-level bash of
  `bash -c 'cmd &'` exits normally and immediately (`exit_code=0`), so the
  timeout branch never fires and the background process stays on the system
  forever. When the direct child exits, the process group must be checked for
  remaining members.
- **A REPL must not take input with `rl.question()` per turn.** It accepts a
  line only at the moment it is called: anything typed while the model is
  running is still read by readline, has no receiver, and is silently dropped
  (all typeahead lost). Feeding input through a pipe is more extreme --
  readline consumes the whole pipe in one go, everything after the first line
  is lost, and then EOF exits outright. The right approach is a persistent
  `'line'` listener plus a queue, decoupling "reading" from "taking". The
  approval question is not stolen by that listener: readline does not emit
  `'line'` while a `question` is pending.
- **Two readlines must never be alive on one stdin.** They fight over input,
  and whichever closes first also swallows the bytes already buffered. The REPL
  and the approval question must share one.
- **The event loop must contain no unbounded read loop and must never block
  writing to stdout.** The former starves under a high-output child, the latter
  freezes under a slow-reading host -- either brings timeouts and reaping to a
  complete halt.

### Windows-specific (all of them hit)

The first four are four variants of one error: **the smoke cases happened to
cover only the simple situations, so the suite was green while the actual work
was entirely broken** -- the same disease as "a security test cannot detect
that the permitted was blocked" above.

- **An AppContainer cannot read the volume root, so `dir` fails everywhere.**
  This one was the most expensive, because it is both severe and misleading.
  `icacls C:\` has no ALL APPLICATION PACKAGES (and a data drive such as `H:\`
  has no AppContainer grant at all), while cmd's `dir` queries volume
  information -- so even `dir /b <your own workspace>` reports "Access is
  denied", while enumerating **the same directory** with .NET's
  `GetFileSystemEntries` works perfectly, as does `type <file>`.
  `vol` is refused likewise; `powershell.exe` cannot validate its cwd and
  quietly falls its provider location back to `C:\`, so relative paths in every
  cmdlet aim at the wrong place (while .NET relative paths are correct -- two
  sets of relative-path semantics in one process).
  Measured with a real model: **all 20 steps went on "where are my files"**,
  with `dir` refused, `cd && dir` refused, `Get-ChildItem` refused, while
  `fs.read` could clearly read files.
  The fix is one minimal ACE on the **volume root**: `(S,X,RA)` -- synchronize
  + traverse + read-attributes, **without `FILE_READ_DATA`** (which on a
  directory is `FILE_LIST_DIRECTORY`), not inherited. So `dir` works while
  `dir C:\` is still refused. `SYNCHRONIZE` must not be omitted -- with only
  `(X,RA)`, `dir` still says "Access is denied".
  - This is a **one-time machine setup, not something the engine stamps on
    every run**. Measured, a single `icacls C:\ /grant` takes **16 seconds**
    (the volume root's children take part in inheritance propagation), and a
    session would grant and revoke, adding 30-odd seconds to every
    `session.open`. The engine only **checks**, and when the grant is missing
    emits a warning carrying the exact fix command, leaving the decision to the
    user:

    ```
    icacls C:\ /grant "*S-1-15-2-1:(S,X,RA)"
    ```

  - PowerShell needs one more step: `Set-Location <absolute path>` is still
    Access denied (it needs the parent directory). What works is
    `New-PSDrive -Root <workspace>` followed by `Set-Location hx:` -- the
    host's `shellCommand` prepends exactly that on the pwsh branch.
  - And a **cautionary tale**: granting traverse rights level by level from the
    volume root down to the workspace was tried once, hoping to fix
    powershell's location along the way. It was a disaster --
    `SetNamedSecurityInfoW` applied to a directory **recomputes inheritance
    across the entire subtree**, so calling it on `C:\Users\<user>` means
    traversing the whole user profile; it hung for ten minutes, and killing it
    partway left ACEs behind in the user's directory. The ancestor chain is not
    a viable route; powershell's location is solved on the host side instead.

- **`DETACHED_PROCESS` silently breaks nested processes' stdout.** To dodge
  conhost joining the Job (see below), `DETACHED_PROCESS` was briefly used in
  place of `CREATE_NO_WINDOW`. The result: `cmd.exe` itself still ran and
  **builtins** like `echo`/`type` still produced output, but any **external**
  program cmd then started (node/ping/powershell) got no stdout at all -- exit
  code 0 or 1, not one byte of output. `CREATE_NO_WINDOW` is required.
- **`cmd.exe` does not use `CommandLineToArgvW`'s quoting rules.** The general
  method writes an embedded quote as `\"`, while cmd treats a backslash as an
  ordinary character. So passing `["cmd","/c","powershell -Command \"...\""]`
  leaves powershell receiving a **string literal**, which it echoes back with
  exit code 0, looking like it "ran fine". The fix is
  `cmd /s /c "<command verbatim>"`: `/s` makes cmd strip only the outermost
  pair of quotes and escape nothing in between.
- **`apply_patch` is bound to report "context not found" on CRLF files.** The
  patch format splits on `\n` while files on Windows are CRLF, so every line
  cut out carries an extra `\r` and a byte-for-byte comparison against the
  patch's context is bound to fail. And even when it matches, rejoining on
  `\n` rewrites the whole file's line endings -- "change one line" produces a
  whole-file diff. Strip `\r` when splitting, remember the original style, and
  restore it when writing back.
- **`SearchPath` must not try "no extension appended" first.** Copying `execvp`
  over means `cmd` first hits `C:\MinGW\msys\1.0\bin\cmd`, an extensionless
  shell script, rather than `System32\cmd.exe`. Append extensions in PATHEXT
  order. Likewise, the minimal environment's PATH must put system directories
  **first**, not merely "present" -- behind MSYS/chocolatey shims, what runs is
  not the program the model thinks it is, and it **succeeds**, just with the
  wrong behaviour.
- **A `.cmd` file must contain no non-ASCII -- including inside `rem`
  comments.** cmd.exe reads batch files in the machine's **OEM code page** (936
  here), not UTF-8. UTF-8 bytes get re-paired as GBK double-bytes, the
  alignment shifts, and an ASCII byte from the middle of a multi-byte sequence
  surfaces as a command separator -- so cmd starts executing fragments of the
  `rem` line. The observed report was
  `'TH' is not recognized as an internal or external command`, entirely
  unrelated to the real problem.
- **AppContainer profiles live in the registry; the directory under
  `%LOCALAPPDATA%\Packages` is created only on demand.** Profiles left behind
  after a kill -9 are reclaimed by the startup sweep, and the first version of
  that sweeper scanned the directory -- so it **ran and cleaned nothing up**,
  with nothing to indicate it. Measured after seven consecutive sessions: not
  one `hx-*` directory under `Packages`, while all seven monikers sat in
  `HKCU\...\AppContainer\Mappings`.
  The registry is the authoritative list; `DeleteAppContainerProfile` cleans
  both. Section H of the smoke suite verifies exactly this: hard-kill to
  produce residue (**asserting the residue really was produced**, or the test
  is empty), then start another engine and check that it was collected.
- **`CreateProcess` requires `LOCALAPPDATA` when creating an AppContainer
  process.** The profile lives under `%LOCALAPPDATA%\Packages\<name>`, and
  without that variable it fails outright -- with the thoroughly misleading
  `ERROR_ENVVAR_NOT_FOUND(203)`, "The system could not find the environment
  option that was entered", whose text never mentions AppContainer. It alone is
  required; `APPDATA`/`USERPROFILE` are not.
- **"What is installed" and "what works inside the sandbox" are two different
  things.** This is the Windows version of "the toolchain in `$HOME` will not
  run", and worse: the test is whether the target directory grants **ALL
  APPLICATION PACKAGES**, which `icacls <dir>` shows at a glance.
  Measured: `C:\Program Files\Git` has it (inherited), while **`C:\Program
  Files\nodejs` does not** -- the Node installer breaks ACL inheritance, so
  `node` cannot start inside the sandbox. Fixing the ACL requires administrator
  rights. Things under `%SystemRoot%\System32` (`cmd`, `powershell`) always
  work.
- **A Job's `ActiveProcesses` cannot be used to decide "are there orphans".**
  Two traps: the direct child stays on the list after exiting (we still hold
  its HANDLE), and a console program drags a `conhost.exe` in. Counting
  directly makes `orphans_killed` permanently true, which renders the field
  useless. Confirm each pid individually, and exclude conhost by **full image
  path**.

---

## Appendix: making hx modify itself (four rounds, as they happened)

hx was used to drive a local 27B model to add cgroup support to hx itself.
**Three of the four rounds produced nothing at all, and each of the three
failures had a different cause -- three of which were real defects in the
harness.**

| Round | Result | Cause | Fix |
|---|---|---|---|
| 1 | 8 commands failed to fork | `RLIMIT_NPROC` hardcoded to 256, while it counts **threads** (on this machine 104 processes = 667 threads) | Changed to "current usage + 512" |
| 2 | 74 reads / nothing produced | read-compact-forget: a 16K window could not hold the working set, and compaction threw away the code just read | Model window 16K -> 32K |
| 3 | 24 system investigations / nothing produced | **The sandbox could not read `/sys/fs/cgroup`** -- the thing to be modified was being blocked by the sandbox | Added `extra_read_paths` |
| 4 | **8 patches, task completed** | — | The world snapshot gained `step: N of M`, plus pressure to produce |

A few things worth recording:

- **Round 3's deadlock was diagnosed by the model itself** (from the log: "Is
  /sys/fs/cgroup in the default read paths? No (only /sys/devices)"). The task
  design was the human's error, not the model's. When an agent has to modify
  the very mechanism constraining it, a bootstrap deadlock appears.
- **Round 4's key was letting the model see its budget.** In the first three
  rounds it behaved as though time were unlimited -- round 3 performed 24
  high-quality system investigations and wrote not one byte. Adding a line of
  `step: 12 of 30` to the world snapshot, plus "if past halfway with no file
  modified, stop investigating and start working", took reads from 29 to 13 and
  patches from 0 to 8.
- **Tighter constraints produced better results**: `--max-steps` **lowered**
  from 40 to 30 is what made it work.
- The code the model produced contained a detail its author had not thought of:
  after writing `pids.max`, read it back to verify -- "the file existing does
  not mean the controller was delegated".
- Two things were fixed during review: the probe permanently left
  `+pids +memory` on the parent cgroup of machines where it succeeded (it now
  restores only what it added); and the probe's cgroup shared a name with the
  session's, so they deleted each other (they are now `hx-probe-<pid>` /
  `hx-session-<pid>`).

---

## License

The engine vendors [nlohmann/json](https://github.com/nlohmann/json) (MIT).
The host depends on Hono (MIT), zod (MIT), and tsx / TypeScript (MIT /
Apache-2.0).
