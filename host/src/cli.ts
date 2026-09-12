// hx —— 命令行入口。
//   tsx src/cli.ts --root /path/to/repo "把 tests 里失败的用例修好"   # 一次性
//   tsx src/cli.ts --root /path/to/repo                              # 交互式（REPL）
import { createInterface, type Interface as Readline } from "node:readline/promises";

import { EngineClient } from "./engine/client.js";
import { defaultEnginePath } from "./platform.js";
import { parsePreset, preset } from "./policy/presets.js";
import type { ApprovalDecision, ApprovalHandler } from "./policy/approval.js";
import { Agent } from "./loop/turn.js";
import { OpenAICompatClient } from "./model/openai_compat.js";
import { ToolRegistry, defaultTools } from "./tools/registry.js";
import type { ThreadEvent } from "./protocol/events.js";

const DEFAULT_HXD = defaultEnginePath("../../engine/build/", import.meta.url);

function parseArgs(argv: string[]) {
  const opts = {
    root: process.cwd(),
    sandbox: "workspace-write",
    net: "deny",
    hxd: process.env["HX_ENGINE"] ?? DEFAULT_HXD,
    maxSteps: 20,
    approval: process.env["HX_APPROVAL"] ?? "cautious",
    readPaths: [] as string[],
    prompt: "",
  };
  const rest: string[] = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--root" && argv[i + 1]) opts.root = argv[++i]!;
    else if (a === "--sandbox" && argv[i + 1]) opts.sandbox = argv[++i]!;
    else if (a === "--net" && argv[i + 1]) opts.net = argv[++i]!;
    else if (a === "--engine" && argv[i + 1]) opts.hxd = argv[++i]!;
    else if (a === "--max-steps" && argv[i + 1]) opts.maxSteps = Number(argv[++i]);
    else if (a === "--approval" && argv[i + 1]) opts.approval = argv[++i]!;
    // 额外只读授权（可重复）。默认集合很窄：$HOME、/sys/fs/cgroup 都不在里面。
    else if (a === "--read-path" && argv[i + 1]) opts.readPaths.push(argv[++i]!);
    else rest.push(a!);
  }
  opts.prompt = rest.join(" ");
  return opts;
}

function render(e: ThreadEvent): void {
  switch (e.type) {
    case "thread.started": {
      const s = e.sandbox;
      const seal = s.enforced ? "sandboxed" : "NOT SANDBOXED";
      console.log(`● session ${e.threadId} — ${s.sandbox}/${s.net} [${seal}, ${s.backend}]`);
      console.log(`  rollout: ${e.rollout}`);
      for (const w of s.warnings) console.log(`  ! ${w}`);
      break;
    }
    case "item.started":
      if (e.item.type === "command_execution") console.log(`▸ $ ${e.item.command}`);
      break;
    case "item.completed":
      switch (e.item.type) {
        case "command_execution":
          console.log(`  ${e.item.status === "completed" ? "✓" : "✗"} exit=${e.item.exitCode ?? "?"}`);
          break;
        case "file_change":
          for (const c of e.item.changes) console.log(`  ✎ ${c.kind} ${c.path}`);
          break;
        case "file_read":
          console.log(`  ▪ read ${e.item.path} (${e.item.lines} lines)`);
          break;
        case "todo_list":
          for (const t of e.item.items) console.log(`  ${t.completed ? "[x]" : "[ ]"} ${t.text}`);
          break;
        case "agent_message":
          console.log(`\n${e.item.text}\n`);
          break;
        case "reasoning":
          if (process.env["HX_SHOW_REASONING"]) {
            console.log(`  \x1b[2m🤔 ${e.item.text.replace(/\n/g, " ").slice(0, 160)}\x1b[0m`);
          }
          break;
        default:
          break;
      }
      break;
    case "approval.requested":
      // 事件只负责显示；真正的提问在 handler 里，避免两处各打印一遍
      break;
    case "approval.resolved":
      console.log(`  ${e.decision === "deny" ? "✗ denied" : "✓ approved"}`);
      break;
    case "context.compacted":
      console.log(`  ⧗ compacted ${e.replacedMessages} messages: ~${e.tokensBefore} → ~${e.tokensAfter} tokens`);
      break;
    case "turn.completed":
      console.log(`● done (in=${e.usage.inputTokens} out=${e.usage.outputTokens} tokens)`);
      break;
    case "turn.failed":
      console.error(`● turn failed: ${e.error.message}`);
      break;
    case "policy.violation":
      console.log(`  ⚠ denied: ${e.op} — ${e.reason}`);
      break;
    default:
      break;
  }
}

/**
 * 交互式审批。非 TTY 时一律拒绝 —— 没人能回答却放行，等于规则不存在。
 *
 * ★ 交互模式下必须**复用** REPL 那个 readline，不能自己再开一个：
 *   同一个 stdin 上两个 readline 会互相抢输入，而且先关掉的那个会把已经
 *   读进缓冲区的字节一起吞掉（粘贴多行时尤其明显）。
 *
 *   传的是个盒子而不是实例，因为 handler 要在 Agent 构造时就位，而那个
 *   readline 得等 session 开完才能建 —— readline 一建出来就开始吃 stdin，
 *   建早了这中间输入的东西没人接，直接丢。
 */
function makeApprovalHandler(box?: { rl?: Readline }): ApprovalHandler | undefined {
  if (!process.stdin.isTTY) return undefined;
  return async (req) => {
    const shared = box?.rl;
    const rl = shared ?? createInterface({ input: process.stdin, output: process.stdout });
    try {
      console.log(`\n  ⚠ 需要授权 — ${req.tool}`);
      for (const line of req.preview.split("\n").slice(0, 12)) console.log(`    ${line}`);
      const ans = (await rl.question("    [y] 允许一次  [a] 总是允许  [n] 拒绝 (默认 n) > ")).trim().toLowerCase();
      const decision: ApprovalDecision = ans === "y" ? "allow_once" : ans === "a" ? "allow_always" : "deny";
      return decision;
    } finally {
      if (!shared) rl.close();
    }
  };
}

const REPL_HELP = [
  "  /help            这份帮助",
  "  /tools           本轮模型真正能看到的工具（策略隐藏体现在这里）",
  "  /sandbox         这个会话的沙箱实况（enforced / backend / 警告）",
  "  /exit, /quit     退出（Ctrl+D 或空提示符下 Ctrl+C 也一样）",
  "",
  "  直接输入任务回车即可。跑到一半 Ctrl+C 会在**阶段边界**中断：",
  "  已经起来的命令交给引擎的超时和 exec.kill 收尾，不会留下半截副作用。",
].join("\n");

/**
 * 交互式多轮。同一个 Agent 实例反复 run()，历史与会话都留着。
 *
 * ★ 整个 REPL 共用**一个** readline（审批提问也用它，见 makeApprovalHandler）。
 *   一开始写成每轮开关一次，结果第二轮直接读到 EOF —— 关掉的那个 readline
 *   把已经进了缓冲区的后续输入一并丢了。
 *
 * ★ 取一行用的是常驻的 'line' 监听 + 队列，而不是每轮 rl.question()。
 *   question() 只在**被调用的那一刻**接一行；模型跑着的时候敲进来的东西
 *   照样被 readline 读走，但没人接，于是无声丢掉。管道喂输入时更极端：
 *   readline 一口气把整个管道读完，第一行之后全丢，然后 EOF 直接退出。
 *   队列把"读"和"取"解耦，顺带就有了 typeahead。
 *   审批提问不会被这个监听抢走 —— readline 在 question 挂起时不发 'line'。
 *
 * ★ Ctrl+C 有两种语义，靠 running 区分：
 *   提示符上 = 退出；任务跑到一半 = agent.abort()（在阶段边界生效，不当场
 *   掐断 —— 半路砍掉工具会留下引擎并不知情的副作用）。
 */
async function repl(agent: Agent, root: string, box: { rl?: Readline }): Promise<number> {
  console.log(`\n  工作区 ${root}`);
  console.log("  输入任务回车执行，/help 看命令。\n");

  const rl = createInterface({ input: process.stdin, output: process.stdout });
  box.rl = rl;

  const queued: string[] = [];
  let waiter: ((v: string | null) => void) | null = null;
  let eof = false;
  let running = false;

  const deliver = (v: string | null): void => {
    if (waiter === null) return;
    const w = waiter;
    waiter = null;
    w(v);
  };
  rl.on("line", (l) => {
    if (waiter !== null) deliver(l);
    else queued.push(l);
  });
  rl.on("close", () => {
    eof = true;
    deliver(null);
  });
  rl.on("SIGINT", () => {
    if (running) {
      console.log("\n  ⚠ 中断中 —— 等当前阶段结束");
      agent.abort();
    } else {
      eof = true;
      deliver(null);
    }
  });

  const nextLine = (): Promise<string | null> => {
    if (queued.length > 0) return Promise.resolve(queued.shift()!);
    if (eof) return Promise.resolve(null);
    rl.setPrompt("\x1b[1m› \x1b[0m");
    rl.prompt();
    return new Promise((res) => {
      waiter = res;
    });
  };

  for (;;) {
    const raw = await nextLine();
    if (raw === null) {
      console.log();
      return 0;
    }
    const line = raw.trim();
    if (!line) continue;

    if (line.startsWith("/")) {
      const cmd = line.slice(1).split(/\s+/)[0]!.toLowerCase();
      if (cmd === "exit" || cmd === "quit") return 0;
      if (cmd === "help") console.log(REPL_HELP);
      else if (cmd === "tools") console.log(`  ${agent.availableTools.join(", ")}`);
      else if (cmd === "sandbox") {
        const s = agent.sandbox;
        if (!s) console.log("  (会话未打开)");
        else {
          console.log(`  ${s.sandbox}/${s.net}  enforced=${s.enforced}  net_enforced=${s.netEnforced}  backend=${s.backend}`);
          for (const w of s.warnings) console.log(`  ! ${w}`);
        }
      } else console.log(`  未知命令 ${line}（/help）`);
      console.log();
      continue;
    }

    running = true;
    try {
      const r = await agent.run(line);
      if (r.stoppedBecause === "max_steps") console.error(`● stopped: hit max steps (${r.steps})`);
    } catch (e) {
      console.error(`● error: ${(e as Error).message}`);
    } finally {
      running = false;
    }
    console.log();
  }
}

async function main(): Promise<number> {
  const opts = parseArgs(process.argv.slice(2));
  // 没给任务：有终端就进交互模式，没终端（管道/CI）就报用法。
  // ★ 不能在非 TTY 下进 REPL —— 那会读到 EOF 立刻退出，看起来像"什么都没干"。
  const interactive = !opts.prompt;
  if (interactive && !process.stdin.isTTY) {
    console.error("usage: hx [--root DIR] [--sandbox MODE] [--net deny|allow] [--read-path PATH]... \"your task\"");
    console.error("       hx [--root DIR] ...                 # 不给任务且有终端时进入交互模式");
    return 64;
  }

  const baseUrl = process.env["HX_BASE_URL"];
  const model = process.env["HX_MODEL"];
  if (!baseUrl || !model) {
    console.error("set HX_BASE_URL and HX_MODEL (any OpenAI-compatible endpoint), optionally HX_API_KEY");
    return 78;
  }

  const engine = new EngineClient(opts.hxd, {
    onEvent: (ev) => {
      if (ev["event"] === "policy.violation") {
        render({ type: "policy.violation", op: String(ev["op"] ?? ""), reason: String(ev["reason"] ?? "") });
      }
    },
    onStderr: (l) => console.error(`  [hxd] ${l}`),
  });

  const rlBox: { rl?: Readline } = {};
  const approvalHandler = makeApprovalHandler(rlBox);
  const agent = new Agent(
    engine,
    new OpenAICompatClient(
      baseUrl,
      process.env["HX_API_KEY"] ?? "",
      model,
      Number(process.env["HX_MAX_TOKENS"] ?? 4096),
      process.env["HX_TEMPERATURE"] ? Number(process.env["HX_TEMPERATURE"]) : undefined,
    ),
    new ToolRegistry(defaultTools()),
    {
      maxSteps: opts.maxSteps,
      onEvent: render,
      rules: preset(parsePreset(opts.approval)),
      ...(approvalHandler ? { approval: approvalHandler } : {}),
      compaction: {
        // 必须与服务端的 n_ctx 对齐。设大了会被服务端截断（悄悄丢历史），
        // 设小了会过早压缩、白白丢信息。llama.cpp 可从 /props 读到 n_ctx。
        contextWindow: Number(process.env["HX_CONTEXT_WINDOW"] ?? 32768),
        reserveTokens: Number(process.env["HX_RESERVE_TOKENS"] ?? 4096),
        keepRecentTokens: Number(process.env["HX_KEEP_RECENT_TOKENS"] ?? 6144),
      },
    },
  );

  try {
    await agent.open({
      roots: [opts.root],
      sandbox: opts.sandbox,
      net: opts.net,
      ...(opts.readPaths.length > 0 ? { extraReadPaths: opts.readPaths } : {}),
    });
    if (interactive) return await repl(agent, opts.root, rlBox);
    const result = await agent.run(opts.prompt);
    if (result.stoppedBecause === "max_steps") {
      console.error(`● stopped: hit max steps (${result.steps})`);
      return 1;
    }
    return 0;
  } catch (e) {
    console.error(`● error: ${(e as Error).message}`);
    return 70;
  } finally {
    rlBox.rl?.close();
    engine.close();
  }
}

process.exitCode = await main();
