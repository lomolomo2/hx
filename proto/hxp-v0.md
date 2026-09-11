# hxp v0 —— hx 引擎协议

> host (TypeScript) ⇄ hxd (C++ 原生引擎)，JSONL over stdio。
> **本文档先于实现。任何实现与本文档不符，改的是实现。**

## 0. 传输

- 每行一个 JSON 对象，UTF-8，`\n` 分隔，**行内不得有裸换行**。
- host → hxd 走 hxd 的 stdin；hxd → host 走 stdout。
- hxd 的 stderr 只用于人类可读的诊断日志，**不参与协议**。
- 单行上限 `8 MiB`；超限 hxd 回 `E_LINE_TOO_LONG` 并丢弃该行（不断连接）。

## 1. 三种消息

| 类型 | 判据 | 方向 |
|---|---|---|
| 请求 Request | 有 `id` 和 `op` | host → hxd |
| 响应 Reply | 有 `reply_to` | hxd → host |
| 事件 Event | 有 `event`，无 `reply_to` | hxd → host |

### Request

```jsonc
{
  "id": "r1",              // host 生成，唯一，用于关联响应
  "op": "exec.start",      // 操作名
  "args": { },             // 操作相关
  "replay": "never"        // 可选："never" | "safe"，默认 "never"
}
```

`replay` 是**崩溃恢复语义**，不是重试策略：
- `safe` —— 该操作无外部副作用或幂等（读文件、查状态），恢复时可安全重跑
- `never` —— 有不可逆副作用（写文件、跑命令），恢复时**绝不重跑**，只能报告中断

### Reply

```jsonc
{"reply_to":"r1","ok":true,"result":{ }}
{"reply_to":"r1","ok":false,"error":{"code":"E_PATH_ESCAPE","message":"...","detail":{ }}}
```

**每个 Request 恰好收到一个 Reply。** 长任务的中间产出走 Event，不走 Reply。

### Event

```jsonc
{"event":"exec.output","cell":"c1","stream":"stdout","data":"..."}
```

## 2. 错误码

| code | 含义 |
|---|---|
| `E_BAD_REQUEST` | JSON 结构不合法 / 缺字段 |
| `E_UNKNOWN_OP` | 未知 op |
| `E_BAD_ARGS` | args 不满足该 op 的 schema |
| `E_PATH_ESCAPE` | 路径 realpath 后不在 workspace roots 内 |
| `E_DENIED` | 被沙箱或策略拒绝 |
| `E_NO_SESSION` | 尚未 `session.open` |
| `E_NO_CELL` | cell 不存在或已回收 |
| `E_LINE_TOO_LONG` | 单行超限 |
| `E_SANDBOX_UNAVAILABLE` | 所需隔离层在本内核不可用 |
| `E_INTERNAL` | 引擎内部错误（应当伴随 stderr 日志） |

## 3. 操作

### 3.1 `ping` — 存活探测
`args`: 无 · `result`: `{"pong":true,"pid":<int>}`

### 3.2 `selftest` — 能力自检
`args`: 无
`result`:
```jsonc
{
  "kernel": "7.0.0-28-generic",
  "landlock": {"available": true, "abi": 6, "fs": true, "net": true},
  "seccomp": {"available": true},
  "userns":  {"available": true},
  "cgroup2": {"available": true, "path": "/sys/fs/cgroup"}
}
```
**用途**：启动时 host 调用一次，把结果写进 rollout 的 `session_meta`——
将来看日志时能知道"这次运行到底有没有真沙箱"。

### 3.3 `session.open` — 开会话并**固化**策略
```jsonc
{"op":"session.open","args":{
  "roots": ["/abs/path/to/workspace"],   // 绝对路径，realpath 后存
  "sandbox": "workspace-write",          // read-only | workspace-write | danger-full-access
  "net": "deny",                         // deny | allow
  "extra_read_paths": ["/sys/fs/cgroup"] // 额外只读授权，见下
}}
```
`result`: `{"session":"s1","effective":{...}}` —— `effective` 是**实际生效**的策略。
若请求 `net:deny` 但内核 Landlock ABI < 4，`effective.net` 为 `"allow"` 且附 `warnings:[...]`。
**引擎不假装做到了做不到的事。**

`extra_read_paths` 是对默认系统只读路径的补充。默认集合故意很窄（不含 `$HOME`、
不含 `/sys/fs/cgroup`），但有些任务确实需要开口：工具链装在 `$HOME`（nvm/rustup/conda），
或任务本身就是关于某个系统接口的。**显式授予好过放宽默认值。**
路径不存在时跳过并在 `effective` 里如实列出，不因此让会话开不起来。

> **策略单调收紧**：`session.open` 后，同一 session 的策略只能变严不能放松（Landlock 本身不可撤销）。
> 放松的唯一方式是开新 session。

### 3.4 `exec.start` — 在沙箱里起进程
```jsonc
{"op":"exec.start","args":{
  "cmd": ["bash","-lc","npm test"],
  "cwd": ".",                 // 相对 roots[0]，经 path_guard 校验
  "env": {"CI":"1"},          // 白名单合并进最小环境，不继承 host 全部 env
  "timeout_ms": 600000,
  "pty": false,               // true 走 forkpty，可跑 REPL / 交互式命令
  "rows": 24, "cols": 120     // 仅 pty 模式有意义
}}
```
`result`: `{"cell":"c1","pty":false}` —— **`cell` 是不透明 ID，协议里任何地方都不出现真实 pid。**

pty 模式下输出不区分 stdout/stderr（终端本来就只有一条流），`exec.output` 事件的
`stream` 字段为 `"pty"`。

### 3.5 `exec.stdin` — 向已有 cell 喂输入
`args`: `{"cell":"c1","data":"y\n","close":false}`
`result`: `{"written":2,"pending":0}`

`pending > 0` 表示内核缓冲已满、只写进去一部分——调用方需要自己决定是否重试余下部分。
`close:true` 关闭 stdin（给 `cat` 之类送 EOF）；pty 模式下无效，因为读写是同一个 fd。

### 3.6 `exec.wait` — 带超时地取输出
```jsonc
{"op":"exec.wait","args":{"cell":"c1","yield_ms":1000,"max_bytes":16384}}
```
`result`: `{"done":false,"data":"...","truncated":true,"exit_code":null}`

**等不到就先回来**，把部分输出交给调用方（对应 `KeWaitForSingleObject` 带 timeout）。
`done:true` 时 `exit_code` 非 null，且该 cell 之后即被回收。

### 3.7 `exec.kill`
`args`: `{"cell":"c1","signal":"TERM"}` · `result`: `{"killed":true}`

### 3.8 `fs.read`
`args`: `{"path":"src/a.ts","offset":0,"limit":500}`（按**行**）
`result`: `{"content":"...","lines":123,"truncated":false}`

### 3.9 `fs.apply_patch`
`args`: `{"patch":"*** Update File: src/a.ts\n@@ ...\n"}`
`result`: `{"changes":[{"path":"src/a.ts","kind":"update"}]}`
**原子性**：全部成功或全部不落盘（tmp + rename）。失败时不得留下半个文件。

### 3.10 `fs.stat` / `fs.glob`
`fs.stat` args `{"path":"..."}` · `fs.glob` args `{"pattern":"src/**/*.ts","limit":1000}`

### 3.11 `log.append` — durable rollout 追加
`args`: `{"record":{ }}` · `result`: `{"seq":42}`
引擎保证：**要么整行写入，要么什么都没写**（先写完整行再 `write`，必要时 `fsync`）。
host 崩溃不影响已落盘的日志。

### 3.12 `policy.set` — 收紧策略
`args`: `{"sandbox":"read-only"}` 或 `{"net":"deny"}` · `result`: `{"effective":{...}}`
只接受**更严**的值；试图放松返回 `E_DENIED`。

## 4. 事件

| event | 字段 | 时机 |
|---|---|---|
| `exec.output` | `cell,stream,data` | 子进程有输出。`stream` 为 `stdout` / `stderr` / `pty` |
| `exec.exit` | `cell,exit_code,signal` | 子进程结束 |
| `policy.violation` | `op,path/target,reason` | 某次操作被沙箱或 path_guard 拒绝 |
| `engine.warning` | `message,detail` | 降级运行（如 Landlock ABI 不足） |

`policy.violation` **必须同时发事件和回错误响应**：
响应是给调用方的，事件是给审计和 UI 的（规矩一：两条流分离）。

## 5. 参数捕获（规范性要求）

引擎收到 Request 后**必须**按此顺序处理，不得调换：

1. 整行读入并解析成 JSON（失败 → `E_BAD_REQUEST`）
2. 按 op 的 schema 校验 `args`（失败 → `E_BAD_ARGS`）
3. **把 args 拷贝进引擎自有内存**，此后不再引用解析缓冲区
4. 路径类参数一律经 `path_guard`（realpath + roots 校验）
5. 执行

第 3 步是防 double-fetch 的关键：**校验过的值和使用的值必须是同一份内存**。

## 6. 版本

握手由 `selftest` 的 `result.proto` 字段承担：`{"proto":"hxp/0"}`。
v0 阶段不保证向后兼容，host 与 hxd 必须同版本构建。
