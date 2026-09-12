# hx

*[English](README.md) · 中文*

一个带 OS 级沙箱的 agent harness。引擎是原生二进制，宿主是 TypeScript。

```
LLM（不可信，高频产生意图）
  ↓ 工具调用 —— 唯一入口
hx-host  (TypeScript)   决定「该不该做」：上下文装配、提示词、策略、编排
  ↓ hxp/0 —— JSONL over stdio，唯一入口
hxd      (C++)          决定「能不能做」：Landlock / seccomp / rlimit / exec / 补丁 / 日志
  ↓
真实世界
```

**核心约束**：`hx-host` 不允许 `import node:fs` 或 `node:child_process`（`test/arch.sh` 强制）。
所有副作用必须过引擎。这样"宿主相对引擎就是用户态"不是一句口号，而是架构事实。

本项目的设计不是凭空来的，四条主线写在代码注释里，这里先给结论：

- **底座调研**：对比 codex（深：沙箱/审批/补丁）、pi（白盒 runtime 库）、opencode（harness 即服务）、DeerFlow 2.0（lead agent + subagent）之后，选择「原生引擎 + TS 宿主」的双进程特权分离。
- **特权边界**：整套安全模型对照 Windows 的 user/kernel 分界——syscall 唯一入口、参数捕获、HANDLE 不透明 ID、access mask、IRP 可拦截、IRQL 阶段约束、UIPI 完整性级别。每条对应关系都在相关源文件的注释里。
- **调度模型**：turn 循环对照 Win32 消息循环（无状态 WndProc = 无状态模型、WM_PAINT 合并 = 上下文压缩）。
- **实施顺序**：先协议后实现；先最小闭环再加工程属性；地基（两条流、append-only 事实来源、完整状态快照）必须 Day 1 定死。

---

## 快速开始

### 构建引擎

```bash
cd engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8
./build/hxd --self-test          # 打印本机隔离能力，退出码 0 = 有真沙箱
```

Linux 与 Windows。`--self-test` 的退出码在两个平台上是同一个意思：
**0 = 这台机器上有真沙箱，2 = 没有**。没有时引擎会如实上报"无文件系统隔离"
而不是假装安全。

- **Linux** 需要内核启用 Landlock（`cat /sys/kernel/security/lsm` 里应有 `landlock`）。
- **Windows** 需要 Windows 10 1809 或更新（AppContainer + ConPTY）。
  用 MSVC 构建（VS2019 16.11 起，需 `/std:c++20`）：

  ```powershell
  cd engine
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build build
  .\build\hxd.exe --self-test
  ```

  典型输出（这台机器有全套隔离）：

  ```json
  {"platform":"windows","kernel":"10.0.26200",
   "filesystem":{"available":true,"backend":"appcontainer"},
   "network":{"available":true,"backend":"appcontainer-capabilities","covers_udp":true},
   "limits":{"available":true,"backend":"job-object","nested_jobs":true,
             "max_processes":true,"max_memory":true,
             "max_file_bytes":false,"max_open_files":false},
   "pty":{"available":true,"backend":"conpty"}}
  ```

  注意 `limits` 里那两个 `false`：Job 对象没有 `RLIMIT_FSIZE` / `RLIMIT_NOFILE`
  的对位，引擎不假装设上了。详见「两个平台的对应关系」。

  **一次性机器设置（要不要做由你决定）**：AppContainer 默认读不了卷根，
  而 `dir` / `Get-ChildItem` 要查卷信息 —— 不加这条，沙箱里**列不了任何目录**
  （读写文件、跑程序都不受影响）。引擎会在 `session.open` 的 warnings 里
  如实提示，并给出确切命令。每个要当 workspace 的盘各做一次，**需要管理员**：

  ```powershell
  icacls C:\ /grant "*S-1-15-2-1:(S,X,RA)"
  ```

  只给「同步 + 遍历 + 读属性」，**不给列内容**、不继承 —— 所以 `dir C:\`
  依然被拒。形状和 Windows 出厂就挂在 `C:\` 上的那条能力 ACE 一样。
  撤销：`icacls C:\ /remove:g "*S-1-15-2-1"`。

### 跑一个任务

```bash
cd host && npm install

# 任意 OpenAI 兼容端点：本机 llama.cpp / DeepSeek / OpenRouter / vLLM …
export HX_BASE_URL=http://127.0.0.1:8080/v1
export HX_MODEL=qwen3.8-27b-uncensored
export HX_CONTEXT_WINDOW=16384          # 必须与服务端 n_ctx 对齐

npx tsx src/cli.ts --root /path/to/repo "把 tests 里失败的用例修好"   # 一次性
npx tsx src/cli.ts --root /path/to/repo                             # 交互式
```

不给任务且 stdin 是终端时进**交互模式**：同一个会话、同一份历史反复对话，
`/help` `/tools` `/sandbox` `/exit`，跑到一半 Ctrl+C 在阶段边界中断当前一轮。
非终端（管道、CI）下不会进 —— 那会立刻读到 EOF，看起来像"什么都没干"。

### Windows：当成一条命令用

> Windows 上的完整说明（装、配、验、排错、沙箱边界）在
> **[docs/windows.zh-CN.md](docs/windows.zh-CN.md)**。不想从源码构建的话，
> [Releases](https://github.com/lomolomo2/hx/releases) 有打好的包，
> 解开加进 PATH 就能用（引擎是原生二进制，宿主需要 Node 20+）。

仓库根的 `hx.ps1` / `hx.cmd` 是个启动器，把这个目录加进 `PATH` 之后：

```powershell
cd C:\path\to\your\repo
hx                                  # 当前目录作工作区，进交互
hx "把 tests 里失败的用例修好"         # 一次性
hx -Net allow -Approval auto -MaxSteps 40 "..."
```

**敲 `hx`（走 `hx.cmd`），不要敲 `.\hx.ps1`。** 默认的 Windows 装机执行策略是
`Restricted`，直接跑 `.ps1` 会被拒（`running scripts is disabled on this system`）。
`hx.cmd` 带着 `-ExecutionPolicy Bypass` 起 pwsh，存在的理由就是这个 ——
所以不需要为了用 hx 去改机器的执行策略。没装 PowerShell 7 时它退回
`powershell.exe`。

端点配置优先级 命令行 > 环境变量 > `~\.hx\config.json`：

```json
{ "baseUrl": "https://192.168.1.241/llm/v1", "model": "qwen3.8-27b-uncensored", "contextWindow": 32768 }
```

自签证书把 PEM 放到 `~\.hx\certs\llm-<主机名>.pem`，启动器会 `NODE_EXTRA_CA_CERTS`
**只**信任它，而不是把整个进程的 TLS 校验关掉。

### 起服务

```bash
npx tsx src/server/main.ts --port 4100

curl -X POST localhost:4100/session -H 'content-type: application/json' \
  -d '{"roots":["/path/to/repo"],"sandbox":"workspace-write","net":"deny"}'
curl -N localhost:4100/session/s1/event                       # SSE 事件流
curl -X POST localhost:4100/session/s1/prompt -H 'content-type: application/json' \
  -d '{"text":"你的任务"}'
```

---

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `HX_BASE_URL` | — | OpenAI 兼容端点，必填 |
| `HX_MODEL` | — | 模型名，必填 |
| `HX_API_KEY` | 空 | 本地模型通常不需要 |
| `HX_MAX_TOKENS` | 4096 | 推理模型给少了 `content` 会是空的 |
| `HX_CONTEXT_WINDOW` | 32768 | **必须与服务端 `n_ctx` 对齐**：设大了会被服务端静默截断，设小了会过早压缩 |
| `HX_RESERVE_TOKENS` | 4096 | 给回复留的余量 |
| `HX_KEEP_RECENT_TOKENS` | 6144 | 末尾多少 token 保持原文 |
| `HX_APPROVAL` | cautious | `auto` / `cautious` / `strict` |
| `HX_ENGINE` | `../engine/build/hxd`（Windows 上是 `hxd.exe`） | 引擎路径 |
| `HX_SHELL` | Linux `bash`，Windows `cmd` | `bash` 工具用哪个 shell。**改了它，工具描述里给模型看的方言名也跟着改** —— 名字叫 bash、底下跑 cmd 而不告诉模型，它会一路发 POSIX 命令然后每条都失败 |
| `HX_SHOW_REASONING` | — | 设了就显示推理模型的思考摘要 |

CLI 参数：`--root` `--sandbox` `--net` `--approval` `--max-steps` `--engine`。

---

## 安全模型

**两层，正交，互不替代。**

| | 管什么 | 能否绕过 | 失败时 |
|---|---|---|---|
| 策略层（宿主） | 该不该做（意图） | 能——它只是 JavaScript | 拦住"技术上做得到但不该做"的事 |
| 沙箱层（内核） | 能不能做（能力） | 不能 | 拦住"模型被注入后想做"的事 |

只有策略没有沙箱 = 一行 JavaScript 挡在攻击者面前。
只有沙箱没有策略 = agent 在授权范围内可以任意破坏。

### 沙箱层做了什么

下面按 Linux 写。Windows 上每一条的对位见「两个平台的对应关系」，
机制不同但**性质相同**：都是 allow-list、都在内核里强制、都不靠黑名单。

- **Landlock**：文件按 allow-list 授权。默认系统只读路径**不含 `$HOME`** —— 这就是 `cat ~/.ssh/id_rsa` 返回 EACCES 的全部原因，不需要任何黑名单。符号链接逃逸自动被挡（Landlock 在解析后的路径上判定）。
  Windows 对位是 AppContainer：进程 token 带一个低权限 package SID，
  只有 DACL 里明确授予了它的对象才放行。用户 profile 默认不授予，
  于是 `type %USERPROFILE%\.ssh\id_rsa` 直接 ACCESS_DENIED —— 同一个机制。
  符号链接/junction 逃逸由 `path_guard` 打开句柄后问内核要最终路径来挡
  （`GetFinalPathNameByHandle`，等价于 `realpath`）。
- **seccomp-bpf**：封掉 `AF_INET`/`AF_INET6` 的 `socket()`（Landlock 只管 TCP，UDP/DNS 是它的盲区），以及 `ptrace`/`mount`/`keyctl`/`bpf` 等与构建任务无关的 syscall。返回 EPERM 而非 KILL——工具能报告错误，模型才会换路走。
- **rlimit**：`RLIMIT_NPROC` = **当前 task 数 + 512**（不能写死绝对值——它数的是**线程不是进程**，本机 104 进程对应 667 线程；曾经写死 256，结果沙箱里每次 fork 都失败）、`RLIMIT_FSIZE` 2 GiB、`RLIMIT_NOFILE` 4096。
- **进程组即所有权**：cell 的直接子进程退出时，若进程组里还有成员，引擎会冻结（SIGSTOP）+ 清扫（SIGKILL）直到组为空。没有这一条，任何 `cmd &` 都会在系统里留下守护进程。
- **施加顺序**：`fork → chdir → landlock_restrict_self → rlimit → seccomp → execve`。
  Landlock 撤销不了已打开的 fd，seccomp 装早了会挡掉自己后面的步骤——**顺序错了等于没做**。
- **装不上沙箱就不执行**（`_exit(126)`），绝不降级为无保护运行。

### 诚实的边界

- **`RLIMIT_NPROC` 按真实 UID 计数、且数的是线程。** 上限只能相对当前用量来设（当前 + 512）。fork 炸弹会被挡住、机器活下来，但同一用户的其它进程在攻击期间也可能 fork 失败。**这是缓解，不是隔离。**
- **cgroup v2 有探测但本机不可用。** `--self-test` 会实际走一遍「启用控制器 → 建子 cgroup → 写 pids.max/memory.max → 读回校验」，并如实回报：

  ```json
  "cgroup2": {"available": true, "usable": false,
    "reason": "enable subtree_control: ...: Device or resource busy"}
  ```

  `available` 只说文件系统在，`usable` 才说限额装得上。普通用户会话里 hxd 与 shell 同处一个 scope，写 `cgroup.subtree_control` 会 `EBUSY`（cgroup v2 的"无内部进程"规则）。把引擎放进独立 scope（`systemd-run --user --scope -p Delegate=yes`）即可用上 —— 那是部署方式的选择，引擎不该自作主张。探测本身无副作用：只还原自己新启用的控制器，用户原有的不动。
  **注意 `Cgroup2Session` 尚未接进 spawn 路径**，目前只有探测与会话原语。
- **`danger-full-access` 模式下没有内核强制**，只剩 `path_guard` 的软校验。
- Linux 与 Windows。macOS Seatbelt 留了接口 seam（`sandbox/confine.hpp`），未实现。

---

## 两个平台的对应关系

安全模型本来就是照着 Windows 的 user/kernel 分界设计的（见开头「特权边界」那条），
所以移到 Windows 上不是"找个近似物凑合"，而是回到它的原型。
每一条的细节都写在对应源文件的注释里。

| 要解决的问题 | Linux | Windows | 哪边更强 |
|---|---|---|---|
| 文件按 allow-list 授权 | Landlock ruleset | AppContainer SID + 目录 ACE | 平手 |
| `$HOME` 默认不可读 | 不加进 ruleset | 用户 profile 不授予 package SID | 平手 |
| 断网 | Landlock(TCP) **+** seccomp(AF_INET，补 UDP 盲区) | 不授予 `internetClient` 能力，WFP 内核拦截 | **Windows**：一层覆盖 TCP+UDP，没有盲区 |
| 进程数上限 | `RLIMIT_NPROC`（按 UID、数线程） | Job `ActiveProcessLimit`（按 Job、数进程） | **Windows**：可写绝对值，不影响同用户其它进程 |
| 内存上限 | 无（cgroup 未接进 spawn 路径） | Job `JobMemoryLimit` | **Windows** |
| 单文件大小 / fd 数上限 | `RLIMIT_FSIZE` / `RLIMIT_NOFILE` | **无对位** | **Linux** |
| 进程组即所有权 | `setsid` + `kill(-pgid)` + 反复清扫 | Job 对象，`TerminateJobObject` 原子终止 | **Windows**：没有"和 fork 速度赛跑"这回事 |
| 引擎自己被 `kill -9` 后 | 子孙被 init 收养，继续活着 | `KILL_ON_JOB_CLOSE`，内核连带收干净 | **Windows** |
| 交互式会话 | `forkpty` | ConPTY（`CreatePseudoConsole`） | 平手 |
| 事件循环 | epoll | IOCP + overlapped 命名管道 | 平手 |
| 降权的时机 | fork 后子进程 `landlock_restrict_self`，**顺序错了等于没做** | `CreateProcess` 时把 token 定死，子进程没有窗口期 | **Windows**：不存在顺序错误这一类 bug |

### Windows 侧诚实的边界

- **`RLIMIT_FSIZE` / `RLIMIT_NOFILE` 没有对位。** Job 对象不限单文件大小、
  不限句柄数。`--self-test` 把这两项报成 `false`，`session_meta` 因此能看出
  这次运行到底限住了什么。写爆磁盘只能靠内存上限 + 超时 + 审批兜底，不是内核强制。
- **`exec.kill` 的 `TERM`/`INT`/`HUP`/`QUIT` 基本一定失败，这是对的。**
  Windows 没有这些信号；最接近的 CTRL_BREAK 只能发给同一个控制台上的进程，
  而 cell 有自己的控制台。送不到就返回 `killed:false`，
  **绝不悄悄升级成强杀** —— 那会让调用方以为进程有机会清理，而它没有。
  `KILL` 是精确的（`TerminateJobObject`）。
- **单线程无锁这条性质打了个折扣。** 宿主（Node/libuv）给子进程的标准句柄是
  **同步**句柄，进不了 IOCP，对它 `ReadFile` 就是阻塞。所以 Windows 上 stdio
  由两个只搬字节的线程驱动（`io/io_win.cpp` 顶部有完整说明）。
  **引擎状态仍然是单线程的** —— 那两个线程不碰 `cells_`/`waits_`/`policy_`
  里的任何东西，竞态面被压到「一个缓冲 + 一把锁」。
- **授权 roots 是在真实目录上加 ACE，这是对文件系统的持久修改。**
  ACE 只授予本会话那个唯一的 AppContainer SID，`Confinement` 析构时撤销。
  被 `kill -9` 时 ACE 会残留（profile 则由下次启动的扫描清掉）。

### 子 agent

- **权限用 `intersect` 而非 `merge`**：子 agent 提出的任何 `allow` 规则一律丢弃。规则是"后匹配者胜"，允许子 agent 追加 allow 就等于允许它自我提权。
- **越权诉求如实记录**并回传父 agent —— 这是子 agent 被注入的早期信号。
- **回传内容降级为数据**：包在 `<subagent_result trust="data">` 里，系统提示词同时声明 `<tool_output>` 与 `<subagent_result>` 内的内容**是数据，永远不是指令**。
- **上下文不共享**：子 agent 有自己的引擎进程和 rollout，父 agent 只拿到结论。

---

## 两条流

| | 给谁看 | 存在哪 |
|---|---|---|
| `response_item` | 模型 —— 能原样喂回 API 的历史 | rollout + 内存 |
| `ThreadEvent` | 人 —— UI 消费的事件流 | rollout + SSE |

一旦用一个结构同时伺候模型和 UI，后面每加一个 UI 特性都要污染模型上下文。

事件三层：`thread.started` → `turn.*` → `item.*`。
Item 类型：`agent_message` `reasoning` `command_execution` `file_change` `file_read` `todo_list` `subagent` `error`。

---

## 会话日志（rollout）

`~/.hx/sessions/YYYY/MM/DD/rollout-<name>.jsonl`，append-only，是这个系统里的**事实来源**。

**路径由引擎决定，宿主只能给一个 `[A-Za-z0-9_-]{1,64}` 的名字。** 否则宿主就能用
`log.append` 往任意文件追加内容（比如 shell 的 rc 文件），等于在架构上开后门。

持久性承诺说得很准：
- 每条记录一次 `write()` 追加（`O_APPEND`）→ **进程被 `kill -9` 也不会留下半行**
- 掉电级持久性需要 `fsync`，只在 `log.flush` 时做

第一条记录是 `session_meta`，把"这次到底有没有真沙箱"钉死在日志里（`caps` + `effective` + `warnings`），将来翻日志不用猜。

读回来：

```bash
node scripts/transcript.mjs --list    # 有哪些会话
node scripts/transcript.mjs s6468     # 某个会话的对话记录
node scripts/transcript.mjs s6468 --full
```

它把每次工具调用和它的结果配好对，并渲染沙箱实况、审批、子 agent 授权与压缩。
注意 rollout **不**包含每一步发给模型的原文：提示词是 `buildPrompt()` 当场装配的，
从不落盘；记下来的是对话本身。

上下文压缩会写一条 `compacted` 记录，带**被替换掉的原文**（`replacement_history`）、摘要、前后 token 数。**压缩是可审计的记录，不是偷偷改历史**——否则 debug 时永远搞不清模型当时看到了什么。

---

## 上下文管理

- **token 估算自校准**：用每次真实 `usage` 反推字符/token 比值。中英文、代码、JSON 的比值差很多，写死常数必然失真。
- **两条压缩不变量**：绝不切散 `assistant(tool_calls)` 与它的 `tool` 结果（切散了服务端会直接拒绝）；最后一组 assistant+tool 永远保留（否则"刚读完的大文件立刻被压掉"，模型以为没读过，再读，死循环）。
- **摘要输入必须包含原始任务**。漏掉它时模型在摘要里写下了 *"The original task statement isn't in the visible history"*，然后丢掉目标开始重新探索。
- **中段截断**：命令输出保头（跑了什么、首个错误）保尾（最终状态、结论、堆栈），砍中间。

---

## 测试

```bash
cd host && npm test          # Linux：十一个套件
```

```powershell
cd host; pwsh test\all.ps1   # Windows：七个套件
```

| 套件 | 验什么 |
|---|---|
| typecheck | TS strict 全开 |
| arch.sh | 宿主不得碰 fs / 起进程（含自检：故意加违规能被抓到） |
| escape/run.sh | 22 项：逃逸拦截 + **可用性（该放的必须放得通）**，`xfail=0` |
| durability/kill9.py | 写日志途中 `kill -9`，10 万行零破损 |
| limits/forkbomb.py | fork 炸弹有界且被清理；`cmd &` 的后台进程不外泄 |
| pty/interactive.py | REPL 交互 + **沙箱在 pty 这条独立路径同样生效** |
| e2e.ts | 多步任务：计划→测试失败→读→补丁→测试通过 |
| compaction.ts | 压缩触发、留痕、不切散工具配对 |
| approval.ts | 规则引擎 + ask 挂起恢复 + allow_always |
| subagent.ts | 权限单调不增、上下文隔离、内容降级 |
| server.ts | HTTP/SSE 全流程 + 经 HTTP 的审批 |

Windows 上 `escape` / `kill9` / `pty` / `forkbomb` 这四个引擎套件没有直接对位
（它们探的是 Landlock / seccomp / RLIMIT 的具体行为），换成了
`engine/tests/windows/smoke.ps1`：**24 项，验的是同一批性质** ——
该放的放得通、该拒的拒掉、进程树不外泄、超时杀得掉、pty 能跑、被强杀之后的残留下次启动清得掉，
只是问的是 AppContainer 和 Job 对象。其中有一项是 Linux 侧验不了的：
`net=deny` 能不能挡住 **UDP/DNS**（Landlock 的盲区，Linux 靠 seccomp 补，
Windows 天然覆盖）。

**两条经验**：

1. 安全测试只验"该拒的拒了"，验不出"该放的没放"。`/dev/null` 授权带错访问位、`RLIMIT_NPROC` 写死 256 —— 两次都是逃逸套件全绿而实际工作全废。逃逸套件因此专门有一节「可用性：该放的必须放得通」。
2. **测试路径必须等于生产路径。** `--sandbox-exec` 少设了 `TMPDIR`、不杀进程组，导致测试在一个真实会话里不存在的环境下通过。这个坑在本项目里踩过三次。

---

## 目录

```
proto/hxp-v0.md            协议规范（先于实现；实现与它不符时，改的是实现）
engine/
  src/platform/            NowMs · 路径形状 · 文件原语（两平台各一份实现）
  src/io/                  Reactor + 非阻塞流（Linux epoll / Windows IOCP）
  src/sandbox/             caps 探测 · policy · confine（平台 seam）
      landlock · seccomp · cgroup        —— Linux
      confine_win（AppContainer + ACE）  —— Windows
  src/exec/                spawn · pty · cell · env · rlimit · proc（进程树所有权）
  src/fs/                  path_guard（唯一把字符串变成路径的地方）· apply_patch
  src/log/                 rollout
  src/engine.cpp           事件循环与 op 分发（单线程 reactor，引擎状态无锁无竞态）
  tests/                   escape · durability · pty · limits  —— Linux
      windows/smoke.ps1                                        —— Windows
host/
  src/platform.ts          宿主侧的平台差异（引擎路径 · shell · 路径比较）
  src/protocol/            ThreadEvent / Item / hxp 类型
  src/engine/client.ts     唯一允许 spawn 引擎的文件
  src/tools/               bash read apply_patch glob grep task todo
  src/context/             budget · compact · truncate
  src/policy/              rules · presets · approval · intersect
  src/loop/turn.ts         四阶段状态机
  src/server/              Hono + SSE
  test/all.sh · all.ps1    两个平台各自的全量回归
hx.ps1 · hx.cmd            Windows 启动器：加进 PATH 后在任意仓库里敲 `hx`
try-hx.ps1                 Windows 试跑入口（-Caps / -Sandbox / -Repl / 离线任务）
scripts/transcript.mjs     把 rollout 读成可读的对话记录（node scripts/transcript.mjs --list）
```

**平台切分的原则：整个文件按平台挑，不在文件里撒 `#ifdef`。**
散落的 `#ifdef` 会让两条路径互相看不见，改一边忘一边；整文件切分至少保证
两边的函数签名必须对得上，编译器会盯着。选择在 `engine/CMakeLists.txt` 里。

`turn.ts` 的四个阶段是显式状态：`assembling → streaming → executing → settling`。
"工具执行到一半触发上下文压缩"是真实的 bug 类别，和"在 `DISPATCH_LEVEL` 访问分页内存"是同一种错误——建成显式状态后，`PhaseError` 会在写错时当场抛出。

---

## 已知的坑（踩过的）

- **nvm / rustup / conda 装在 `$HOME` 的工具链在沙箱里跑不了**，因为 `$HOME` 默认不可读。这不是 bug 是设计正确的表现，但要把路径显式加进 roots。
- **`HX_CONTEXT_WINDOW` 必须与服务端 `n_ctx` 对齐**。llama.cpp 可从 `/props` 读到。
- **Python 的 `.pyc` 缓存会骗你**：改动若字节数不变且落在同一秒内，`(mtime秒, size)` 校验会判定缓存有效——agent 改对了却仍报失败。测试用 `python3 -B`。
- **`bash -lc` 会读 `~/.profile`**，而 `$HOME` 不可读 → 每条命令带一行噪音。用 `bash -c`。
- **`RLIMIT_NPROC` 数的是线程不是进程。** 绝不能写死绝对值：本机 104 个进程对应 667 个线程，写死 256 会让沙箱里每次 `fork` 都失败。
- **cell 必须拥有它的进程组。** `bash -c 'cmd &'` 的顶层 bash 会立刻正常退出（`exit_code=0`），超时分支因此不触发，后台进程永远留在系统里。直接子进程退出时必须检查进程组是否还有成员。
- **REPL 取输入不能每轮 `rl.question()`。** 它只在被调用的那一刻接一行：模型跑着的时候敲进来的东西 readline 照样读走，但没人接，于是无声丢掉（typeahead 全没）。管道喂输入更极端——readline 一口气读完整个管道，第一行之后全丢，然后 EOF 直接退出。正确做法是常驻 `'line'` 监听 + 队列，把"读"和"取"解耦。审批提问不会被这个监听抢走：readline 在 `question` 挂起期间不发 `'line'`。
- **同一个 stdin 上不能同时活着两个 readline。** 会互相抢输入；先关掉的那个还会把已进缓冲区的字节一并吞掉。REPL 与审批提问必须共用一个。
- **事件循环里不能有无界读循环、也不能阻塞写 stdout。** 前者会被高产出的子进程饿死，后者会被读得慢的宿主冻住 —— 两种都会让超时与回收全部停摆。

### Windows 特有的（都踩过）

前四条是同一类错误的四个变体：**冒烟用例恰好只覆盖了简单情况，于是套件全绿而实际工作全废**
—— 和 README 上面那条「安全测试验不出该放的没放」是同一个病。

- **AppContainer 读不了卷根，于是 `dir` 全线失败。** 这条最贵，因为它同时具备
  「症状严重」和「症状误导」两个属性。
  `icacls C:\` 里没有 ALL APPLICATION PACKAGES（数据盘如 `H:\` 更是一条
  AppContainer 授权都没有），而 cmd 的 `dir` 会去查卷信息 —— 结果是连
  `dir /b <自己的工作区>` 都报 "Access is denied"，而**同一个目录**用 .NET 的
  `GetFileSystemEntries` 枚举完全正常，`type <文件>` 也完全正常。
  `vol` 同样被拒；`powershell.exe` 校验不了 cwd，把 provider location 悄悄退回
  `C:\`，于是所有 cmdlet 的相对路径都指错地方（而 .NET 的相对路径是对的 ——
  同一个进程里两套相对路径语义）。
  实测拿真模型跑任务：**20 步全耗在「我的文件到底在哪」上**，
  `dir` 拒、`cd && dir` 拒、`Get-ChildItem` 拒，而 `fs.read` 明明读得到文件。
  解法是在**卷根**上加一条最小 ACE：`(S,X,RA)` —— 同步 + 遍历 + 读属性，
  **不给 `FILE_READ_DATA`**（目录上它就是 `FILE_LIST_DIRECTORY`），不继承。
  于是 `dir` 能用，而 `dir C:\` 依然被拒。`SYNCHRONIZE` 不能漏 ——
  只给 `(X,RA)` 的话 `dir` 照样 "Access is denied"。
  · 这是**一次性的机器设置，不是引擎每次去打的**。实测 `icacls C:\ /grant`
    单次要 **16 秒**（卷根的子项要参与继承传播），而会话要 grant + revoke 两次，
    于是每个 `session.open` 平白多出 30 多秒。引擎改成只**检查**，
    缺了就发一条带确切修复命令的 warning，授权与否由用户决定：

    ```
    icacls C:\ /grant "*S-1-15-2-1:(S,X,RA)"
    ```

  · PowerShell 还要多一步：`Set-Location <绝对路径>` 依然 Access denied
    （它要访问父目录）。可行的是 `New-PSDrive -Root <工作区>` 再
    `Set-Location hx:` —— 宿主的 `shellCommand` 给 pwsh 分支自动加了这一段。
  · 顺带一条**反面教训**：一度试过「从卷根到工作区逐级给遍历权」，
    想顺便修好 powershell 的 location。结果是灾难 —— `SetNamedSecurityInfoW`
    作用在一个目录上会**向整个子树重算继承**，在 `C:\Users\<user>` 上调它
    等于遍历整个用户 profile，直接挂住十分钟，而且中途被杀会在用户目录上
    留下 ACE。祖先链这条路不能走；powershell 的 location 改在宿主侧解决
    （`shellCommand` 给它加一句 `Set-Location ([Environment]::CurrentDirectory)`）。

- **`DETACHED_PROCESS` 会静默废掉嵌套进程的 stdout。** 为了躲开 conhost
  混进 Job（见下条），一度用 `DETACHED_PROCESS` 代替 `CREATE_NO_WINDOW`。
  结果：`cmd.exe` 自己照跑，`echo`/`type` 这些**内建**命令也照常有输出，
  但 cmd 再去起的任何**外部**程序（node/ping/powershell）全部拿不到 stdout，
  退出码 0 或 1，一个字节都不输出。必须用 `CREATE_NO_WINDOW`。
- **`cmd.exe` 不用 `CommandLineToArgvW` 的引号规则。** 通用拼法把内嵌引号写成
  `\"`，而 cmd 把反斜杠当普通字符。于是
  `["cmd","/c","powershell -Command \"...\""]` 传过去，powershell 收到的是个
  **字符串字面量**，它把命令原文回显出来、退出码 0，看起来"跑成功了"。
  解法是 `cmd /s /c "<原样命令>"`：`/s` 让 cmd 只剥掉最外层一对引号，
  中间一个都不转义。
- **`apply_patch` 在 CRLF 文件上必然 "context not found"。** 补丁按 `\n` 分行，
  而 Windows 上的文件是 CRLF，切出来的每行都多一个 `\r`，与补丁上下文逐字节
  比较必然失败。而且就算匹配上，按 `\n` 回写会把整个文件的行尾改掉 ——
  「改一行」产生全文件 diff。必须拆行时剥 `\r`、记住原风格、回写时还原。
- **`SearchPath` 不能先试「不补扩展名」。** 照搬 `execvp` 的话，`cmd` 会先命中
  `C:\MinGW\msys\1.0\bin\cmd` 这个无扩展名的 shell 脚本，而不是
  `System32\cmd.exe`。要按 PATHEXT 顺序补扩展名。
  同理，最小环境里的 PATH 必须把系统目录排在**最前面**，不是「在就行」——
  排在 MSYS/chocolatey 的 shim 后面，跑的就不是模型以为的那个程序，
  而且它会**成功**，只是行为不对。
- **`.cmd` 文件里不能有非 ASCII —— 包括 `rem` 注释里的。** cmd.exe 按机器的
  **OEM 代码页**（本机 936）读批处理文件，不是 UTF-8。UTF-8 的中文字节被当成
  GBK 双字节重新配对，配对一错位，多字节序列中间的某个 ASCII 字节就暴露成了
  命令分隔符 —— cmd 于是开始执行 `rem` 行的碎片。实测报的是
  `'TH' is not recognized as an internal or external command`，
  和真正的问题毫无关系。注释要写中文就写在 `.ps1` 里。
- **AppContainer profile 在注册表里，`%LOCALAPPDATA%\Packages` 下的目录是按需才建的。**
  被 kill -9 之后留下的 profile 要靠启动清扫回收，而第一版清扫器扫的是那个目录 ——
  于是它**跑了，但什么都没清掉**，没有任何迹象。实测连跑七个会话之后
  `Packages` 下一个 `hx-*` 目录都没有，而
  `HKCU\...\AppContainer\Mappings` 里七条 moniker 全在。
  权威列表是注册表；`DeleteAppContainerProfile` 两边都清。
  smoke 的 H 节专门验这个：先强杀出一条残留（并**断言残留确实产生了**，
  否则这条测试是空的），再起一个引擎看它有没有被收掉。
- **`CreateProcess` 建 AppContainer 进程时必须有 `LOCALAPPDATA`。**
  profile 落在 `%LOCALAPPDATA%\Packages\<name>` 下，少了这个变量直接失败，
  报的还是极具误导性的 `ERROR_ENVVAR_NOT_FOUND(203)`「找不到输入的环境选项」——
  错误码完全没提 AppContainer。只有它是必需的，`APPDATA`/`USERPROFILE` 都不用。
- **"装了什么"和"沙箱里能用什么"是两件事。** 这是 Windows 版的
  「`$HOME` 里的工具链跑不了」，而且更麻烦：判据是目标目录有没有给
  **ALL APPLICATION PACKAGES** 授权，`icacls <dir>` 一看便知。
  实测 `C:\Program Files\Git` 有（继承来的），而 **`C:\Program Files\nodejs`
  没有** —— Node 安装程序断开了 ACL 继承，所以 `node` 在沙箱里起不来。
  补 ACL 需要管理员权限。`%SystemRoot%\System32` 下的东西（`cmd`、
  `powershell`）永远可用。
- **Job 的 `ActiveProcesses` 不能拿来判断「还有没有孤儿」。** 两个坑：
  直接子进程退出后仍留在名单里（我们还持有它的 HANDLE），
  以及控制台程序会带一个 `conhost.exe` 进来。直接计数的话
  `orphans_killed` 永远是 true，这个字段就废了。
  要逐个 pid 确认是否真在跑，并按**完整镜像路径**排除 conhost。

---

## 附：让 hx 改自己（四轮实录）

用 hx 驱动本地 27B 模型，给 hx 自己加 cgroup 支持。**四轮里前三轮全是 0 产出，而三次失败各有不同病因，其中三个是 harness 的真实缺陷。**

| 轮次 | 结果 | 病因 | 修复 |
|---|---|---|---|
| 1 | 8 条命令 fork 失败 | `RLIMIT_NPROC` 写死 256，而它数的是**线程**（本机 104 进程 = 667 线程） | 改成「当前用量 + 512」 |
| 2 | 74 次读 / 0 产出 | read-compact-forget：16K 窗口装不下工作集，压缩把刚读的代码扔了 | 模型窗口 16K→32K |
| 3 | 24 次系统调查 / 0 产出 | **沙箱读不到 `/sys/fs/cgroup`** —— 要改的东西正被沙箱挡着 | 新增 `extra_read_paths` |
| 4 | **8 次补丁，任务完成** | — | 世界快照加 `step: N of M` + 产出压力 |

几条值得记下的：

- **第 3 轮的死结是模型自己诊断出来的**（日志原话："Is /sys/fs/cgroup in the default read paths? No (only /sys/devices)"）。任务设计错误在人，不在模型。当 agent 要改的正是约束它自己的机制时，会出现自举死结。
- **第 4 轮的关键是让模型看见预算。** 前三轮它表现得像时间无限——第 3 轮做了 24 次高质量系统调查却一个字节没写。世界快照里加一行 `step: 12 of 30`，再加一句"过半还没改过文件就停止调查、立刻动手"，读取 29→13、补丁 0→8。
- **更紧的约束产出更好**：`--max-steps` 从 40 **降到** 30 反而成了。
- 模型交出的代码有个我没想到的细节：写完 `pids.max` 要**读回校验**——"文件存在不等于控制器已下发"。
- review 时我修了它两处：探测在成功的机器上会把 `+pids +memory` 永久留在父 cgroup（现在只还原自己加的）；探测与会话的 cgroup 同名会互删（现在分别叫 `hx-probe-<pid>` / `hx-session-<pid>`）。

---

## License

引擎 vendor 了 [nlohmann/json](https://github.com/nlohmann/json)（MIT）。
宿主依赖 Hono（MIT）、zod（MIT）、tsx / TypeScript（MIT / Apache-2.0）。
