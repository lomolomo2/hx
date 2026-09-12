# 在 Windows 上跑 hx

*[English](windows.md) · 中文*

hx 是一个带 OS 级沙箱的 agent harness。模型跑出来的命令在
**AppContainer + Job 对象**里执行：读不到你的用户目录、默认断网、
超时杀得掉、后台进程跑不掉。这不是约定，是内核强制的。

Windows 上的实现是原生的，不是 WSL、不是容器、不是兼容层。

---

## 目录

- [装](#装) · [配](#配) · [跑](#跑) · [交互模式](#交互模式)
- [一次性机器设置：卷根 ACE](#一次性机器设置卷根-ace)
- [自己验一遍](#自己验一遍)
- [沙箱边界](#沙箱边界)
- [出问题时](#出问题时)
- [从源码构建](#从源码构建)

---

## 装

需要：**Windows 10 1809 或更新**（AppContainer + ConPTY），**Node.js 20+**。
引擎是原生二进制没有运行时依赖，宿主是 JS 所以要 Node。

1. 从 [Releases](https://github.com/lomolomo2/hx/releases) 下 `hx-windows-x64-<版本>.zip`
2. 解到一个固定位置，比如 `C:\tools\hx`
3. 把那个目录加进 **用户 PATH**

加 PATH 有个坑值得单说。如果你的用户 PATH 里有 `%NVM_HOME%`、
`%USERPROFILE%` 这类占位符（注册表类型是 `REG_EXPAND_SZ`），
**不要**用 `[Environment]::SetEnvironmentVariable(...,"User")` ——
它会把值写成 `REG_SZ`，占位符变成字面量，nvm 切 node 版本就静默失效了，
而且症状跟 PATH 看起来毫无关系。要么用系统设置的图形界面，要么：

```powershell
$k = Get-Item "HKCU:\Environment"
$old = $k.GetValue("Path", "", [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
Set-ItemProperty "HKCU:\Environment" Path ($old.TrimEnd(';') + ";C:\tools\hx") -Type ExpandString
```

改完**开一个新终端**，已经开着的读不到。

验证装好了：

```powershell
hx -Caps
```

退出码 0 = 这台机器上有真沙箱。

### 敲 `hx`，不要敲 `.\hx.ps1`

默认的 Windows 装机执行策略是 `Restricted`，直接跑 `.ps1` 会被拒：

```
.\hx.ps1 : File ...\hx.ps1 cannot be loaded because running scripts is
disabled on this system.
```

`hx.cmd` 带着 `-ExecutionPolicy Bypass` 起 PowerShell，存在的理由就是这个。
**你不需要为了用 hx 去改机器的执行策略。** 没装 PowerShell 7 时它退回
`powershell.exe`。

---

## 配

任何 OpenAI 兼容端点都行：本机 llama.cpp / vLLM / DeepSeek / OpenRouter …

优先级 **命令行 > 环境变量 > `~\.hx\config.json`**。日常用配置文件最省事：

`C:\Users\<你>\.hx\config.json`

```json
{
  "baseUrl": "https://192.168.1.241/llm/v1",
  "model": "qwen3.8-27b-uncensored",
  "contextWindow": 32768,
  "apiKey": ""
}
```

`contextWindow` **必须与服务端的 `n_ctx` 对齐**。设大了会被服务端静默截断
（历史悄悄丢，模型开始重复问已经答过的事）；设小了会过早压缩、白扔信息。
llama.cpp 可以从 `/props` 读到真实值。

### 自签证书

Node 的 fetch 会以 `DEPTH_ZERO_SELF_SIGNED_CERT` 直接拒掉自签端点。
把 PEM 放到：

```
C:\Users\<你>\.hx\certs\llm-<主机名>.pem
```

例如端点是 `https://192.168.1.241/llm/v1`，文件就叫 `llm-192.168.1.241.pem`。
启动器会自动找到它并通过 `NODE_EXTRA_CA_CERTS` **只**信任这一张。

不要用 `NODE_TLS_REJECT_UNAUTHORIZED=0` —— 那是把整个进程的 TLS 校验关掉，
顺带让它对任何中间人都不设防，代价远超"连上这一台机器"。

---

## 跑

```powershell
cd C:\path\to\your\repo

hx                                   # 交互模式
hx "把 tests 里失败的用例修好"          # 一次性，跑完就退
hx -Net allow -Approval auto -MaxSteps 40 "..."
```

当前目录就是工作区（可以用 `-Root` 指别处）。**沙箱只让它碰这一棵树。**

常用参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `-Root` | 当前目录 | 工作区 |
| `-Net` | `deny` | `deny` / `allow`。deny 是内核强制，TCP 和 UDP 都挡 |
| `-Sandbox` | `workspace-write` | `read-only` / `workspace-write` / `danger-full-access` |
| `-Approval` | `cautious` | `auto` / `cautious` / `strict` |
| `-MaxSteps` | 20 | 一轮最多几步 |
| `-ReadPath` | — | 额外只读授权，可重复。工具链装在别处时要用 |
| `-BaseUrl` `-Model` `-ApiKey` `-ContextWindow` | 见 config.json | 临时覆盖 |
| `-CaCert` | 自动发现 | 指定 PEM |

`-Approval auto` 表示不问你、全放行 —— 只在你信任这个工作区时用。
默认的 `cautious` 会在写文件、跑可疑命令前停下来问。

---

## 交互模式

不给任务且 stdin 是终端时进交互模式。**同一个会话、同一份历史**跨轮保留，
不是每次重来。

```
› 先看看这个仓库的测试怎么组织的
› 那把 parser 那几个失败的修掉
› /sandbox
```

| 命令 | 作用 |
|---|---|
| `/help` | 帮助 |
| `/tools` | 本轮模型**真正**能看到的工具（策略隐藏体现在这里） |
| `/sandbox` | 这个会话的沙箱实况：enforced / backend / 警告 |
| `/exit` `/quit` | 退出（Ctrl+D、或空提示符上 Ctrl+C 也一样） |

**跑到一半 Ctrl+C** 会在*阶段边界*中断当前这一轮，不当场掐断 ——
半路砍掉工具会留下引擎并不知情的副作用。已经起来的命令交给引擎的
超时和 `exec.kill` 收尾。

管道喂输入（CI、脚本）不会进交互模式，会报用法 —— 那种情况下它会立刻
读到 EOF，看起来像"什么都没干"。

---

## 一次性机器设置：卷根 ACE

**症状**：沙箱里 `dir` / `Get-ChildItem` 全线失败，而读文件、写文件、
跑程序都正常。模型会一直在"我的文件到底在哪"上打转。

**原因**：AppContainer 默认穿不过卷根（`C:\`），而 cmd 的 `dir` 要查卷信息。

**解法**（每个要当工作区的盘做一次，**需要管理员**）：

```powershell
icacls C:\ /grant "*S-1-15-2-1:(S,X,RA)"
```

`(S,X,RA)` = 同步 + 遍历 + 读属性。**不给 `FILE_READ_DATA`**（目录上那就是
"列出内容"），不继承 —— 所以 `dir C:\` 在沙箱里**依然被拒**，
只是不再挡住你自己工作区的 `dir`。形状和 Windows 出厂就挂在 `C:\` 上的
那条能力 ACE 一样。

`SYNCHRONIZE` 不能漏：只给 `(X,RA)` 的话 `dir` 照样 "Access is denied"。

撤销：

```powershell
icacls C:\ /remove:g "*S-1-15-2-1"
```

**引擎只检查，不替你改。** 缺了它会在 `session.open` 的 warnings 里
如实提示并给出确切命令。这是刻意的：实测 `icacls C:\ /grant` 单次要 **16 秒**
（卷根的子项要参与继承传播），会话要 grant + revoke 两次，
于是每个 `session.open` 平白多出 30 多秒。而且它是对用户磁盘的持久修改,
不该由一个每次运行都发生的动作来做。

数据盘（`D:\`、`H:\` …）通常一条 AppContainer 授权都没有,同样要做一次。

---

## 自己验一遍

别信文档，自己跑。

### 1. 隔离能力自检

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

注意 `limits` 里那两个 `false` —— Job 对象没有 `RLIMIT_FSIZE` /
`RLIMIT_NOFILE` 的对位，引擎不假装设上了。**这份报告是探出来的不是写死的**：
它真的去建一个 AppContainer profile 再删掉、真的设一条 Job 限额再读回来。

### 2. 沙箱实况（需要仓库，不在发布包里）

```powershell
pwsh engine\tests\windows\smoke.ps1
```

24 项，既验"该拒的拒了"，也验"该放的必须放得通"。后者是重点：
这个套件曾经 18/18 全绿，而 `dir` 在沙箱里**全线失败** ——
因为它当时只测了 `echo`、`type`、写文件。安全测试天然验不出"该放的没放"。

### 3. 离线跑一个完整任务

```powershell
pwsh try-hx.ps1
```

起一个假的 OpenAI 兼容端点驱动多步任务（读 → 改 → 跑测试 → 复核）。
只替换"下一步做什么"这个决策，提示词、策略、审批、引擎调用全走真实路径。

---

## 沙箱边界

### 拦得住的

| | 机制 |
|---|---|
| 读不到 `%USERPROFILE%`（`.ssh`、浏览器密码、令牌） | AppContainer：profile 没给 package SID 授权 |
| 写不出工作区 | 同上，只有 roots 上有 ACE |
| 断网，**TCP 和 UDP 都断** | 不授予 `internetClient` 能力，WFP 在内核里拦 |
| 进程炸弹 | Job `ActiveProcessLimit` = 512 |
| 内存 | Job `JobMemoryLimit` = 4 GiB |
| 后台进程跑不掉 | `TerminateJobObject`，原子的 |
| 引擎被 kill -9 也不留孤儿 | `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` |
| junction / 符号链接逃逸 | 开句柄后问内核要最终路径（`GetFinalPathNameByHandle`） |

网络这条 Windows 比 Linux 强：一层能力覆盖 TCP + UDP，没有 DNS 盲区
（Linux 的 Landlock 只管 TCP，要再叠一层 seccomp 才能补上）。

### 拦不住的，或者诚实的缺口

- **没有 `RLIMIT_FSIZE` / `RLIMIT_NOFILE` 的对位。** Job 对象没这两样。
  磁盘写满和句柄耗尽**挡不住**，`--self-test` 里那两个 `false` 就是在说这件事。
- **`exec.kill` 的 TERM / INT 返回 `killed:false`。** Windows 没有信号，
  引擎不会偷偷升级成硬杀 —— 那会绕过被杀进程的清理逻辑。要真杀用 KILL。
- **`danger-full-access` 模式下没有内核强制**，只剩路径校验。
- **策略层（该不该做）是 JavaScript，能绕。** 它和沙箱层（能不能做）
  是两回事，互不替代：只有策略 = 一行 JS 挡在攻击者面前；
  只有沙箱 = agent 在授权范围内可以任意破坏。
- **roots 上的 ACE 是对磁盘的真实修改**，正常退出会撤销；
  被 kill -9 的话下一次启动会自己清掉残留（profile 和 ACE 都清）。
- **沙箱会在工作区里留下一个 `AppData\` 目录。** 因为它把 `USERPROFILE`
  指向工作区根（这样 `$HOME` 是可读的，工具也不会去写你真实的用户目录），
  而 Windows 把 AppContainer 自己的重定向写入放在
  `%USERPROFILE%\AppData\Local\Packages\<容器名>\AC\` 下 ——
  于是那些东西落进了工作区。内容很小（典型是 PowerShell 的启动缓存），
  不会跑到工作区外面，但**不会自动清理**，会按会话累积。
  把 `AppData/` 加进你的 `.gitignore`。

---

## 出问题时

### `running scripts is disabled on this system`

你敲的是 `.\hx.ps1`。敲 `hx`（或 `.\hx.cmd`）。见上面[那一节](#敲-hx不要敲-hxps1)。

### 沙箱里 `dir` 报 Access is denied

卷根 ACE 没做。见[一次性机器设置](#一次性机器设置卷根-ace)。
引擎在 `session.open` 的 warnings 里已经给了确切命令。

### 模型一直发 POSIX 命令（`ls`、`cat`、`grep`）然后每条都失败

默认 shell 是 `cmd.exe`，工具描述里也是这么告诉模型的。
装了 Git Bash 的话可以 `$env:HX_SHELL = "C:\Program Files\Git\bin\bash.exe"`。
**改了它工具描述会跟着改** —— 名字叫 bash 底下却跑 cmd 而不告诉模型，
它会一路发 POSIX 命令然后每条都失败。

### PowerShell 里相对路径指到了 `C:\`

已经处理了，但值得知道症状：进程 cwd 是对的（`[Environment]::CurrentDirectory`
没问题，.NET 的相对路径也没问题），但 PowerShell 的 **provider location**
退回了 `C:\`，于是 cmdlet 的相对路径指向错误的地方 —— 同一个进程里
两套相对路径语义。原因是 `Set-Location` 要访问目标的**父目录**，
而沙箱只授权工作区本身。解法是 `New-PSDrive` 直接以工作区为 root
建一个驱动器，不走父链。`hx` 已经自动加了这一段。

### 某个程序在沙箱里起不来

"装了什么"和"沙箱里能用什么"是两件事。判据是目标目录有没有给
**ALL APPLICATION PACKAGES** 授权：

```powershell
icacls "C:\Program Files\nodejs"
```

实测 `C:\Program Files\Git` 有（继承来的），而 `C:\Program Files\nodejs`
**没有** —— Node 安装程序断开了 ACL 继承。`%SystemRoot%\System32` 下的
东西（`cmd`、`powershell`）永远可用。

补 ACL 要管理员权限;或者把那个目录用 `-ReadPath` 显式加进来。

### 装在 `%USERPROFILE%` 下的工具链跑不了

nvm / rustup / conda 之类。这**不是 bug，是设计正确的表现** ——
用户目录默认不可读。要用就显式授权：

```powershell
hx -ReadPath "$env:USERPROFILE\.cargo" "..."
```

### 模型跑满步数没有进展

先看 `contextWindow` 是不是和服务端 `n_ctx` 对齐了。不对齐的话服务端会
静默截断历史，模型会忘掉自己刚做过什么。

### 会话日志

每次运行都有一份 append-only 的 JSONL，在
`%USERPROFILE%\.hx\sessions\<年>\<月>\<日>\rollout-*.jsonl`。

**第一条记录钉死了"这次到底有没有真沙箱"**：

```powershell
$r = Get-ChildItem "$env:USERPROFILE\.hx\sessions" -Recurse -Filter "rollout-*.jsonl" |
     Sort-Object LastWriteTime | Select-Object -Last 1
(Get-Content $r.FullName -TotalCount 1 | ConvertFrom-Json).payload.effective
```

---

## 从源码构建

需要 **VS2019 16.11+**（要 `/std:c++20`）和 Node 20+。

```powershell
git clone https://github.com/lomolomo2/hx
cd hx

pwsh engine\build-win.ps1        # 找 VS、用它自带的 cmake + Ninja
cd host; npm install; cd ..

.\hx.cmd -Caps
```

`hx.ps1` 认得出自己在工作树里还是在发布包里：工作树下走 tsx 跑 TypeScript
源码（改完立刻生效），发布包里走打好的 bundle。

跑全套回归：

```powershell
pwsh engine\tests\windows\smoke.ps1     # 引擎，24 项
pwsh host\test\all.ps1                  # 宿主：typecheck / arch / e2e / 压缩 / 审批 / 子 agent / server
```

自己打一个包：

```powershell
pwsh scripts\package-win.ps1 -Version v0.2.0
```

打包脚本会在压缩之前跑一遍 `hxd --self-test`，退出码非 0 就拒绝出包 ——
发一个连自检都过不去的二进制，等于把"有沙箱"这句话的举证责任推给用户。

---

## 和 Linux 的对应关系

机制不同，**性质相同**：都是 allow-list、都在内核里强制、都不靠黑名单。

| 要解决的问题 | Linux | Windows |
|---|---|---|
| 文件按 allow-list 授权 | Landlock ruleset | AppContainer SID + 目录 ACE |
| 断网 | Landlock(TCP) + seccomp(AF_INET) | 不授予 `internetClient`（一层覆盖 TCP+UDP） |
| 进程数上限 | `RLIMIT_NPROC`（按 UID、数线程） | Job `ActiveProcessLimit`（按 Job、数进程） |
| 内存上限 | 无（cgroup 未接进 spawn） | Job `JobMemoryLimit` |
| 收掉整棵进程树 | setsid + kill(-pgid) + 反复清扫 | `TerminateJobObject`（原子） |
| 伪终端 | forkpty | ConPTY |
| 事件循环 | epoll | IOCP + overlapped 命名管道 |

更多细节在仓库的 [README](../README.md)，以及各个源文件的注释里。
