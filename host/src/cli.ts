// hx —— 命令行入口。
//   tsx src/cli.ts --root /path/to/repo "把 tests 里失败的用例修好"
import { createInterface } from "node:readline/promises";

import { EngineClient } from "./engine/client.js";
import { parsePreset, preset } from "./policy/presets.js";
import type { ApprovalDecision, ApprovalHandler } from "./policy/approval.js";
import { Agent } from "./loop/turn.js";
import { OpenAICompatClient } from "./model/openai_compat.js";
import { ToolRegistry, defaultTools } from "./tools/registry.js";
import type { ThreadEvent } from "./protocol/events.js";

const DEFAULT_HXD = new URL("../../engine/build/hxd", import.meta.url).pathname;

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
      console.log(`● session ${e.threadId} — ${s.sandbox}/${s.net} [${seal}, landlock abi ${s.landlockAbi}]`);
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

/** 交互式审批。非 TTY 时一律拒绝 —— 没人能回答却放行，等于规则不存在。 */
function makeApprovalHandler(): ApprovalHandler | undefined {
  if (!process.stdin.isTTY) return undefined;
  return async (req) => {
    const rl = createInterface({ input: process.stdin, output: process.stdout });
    try {
      console.log(`\n  ⚠ 需要授权 — ${req.tool}`);
      for (const line of req.preview.split("\n").slice(0, 12)) console.log(`    ${line}`);
      const ans = (await rl.question("    [y] 允许一次  [a] 总是允许  [n] 拒绝 (默认 n) > ")).trim().toLowerCase();
      const decision: ApprovalDecision = ans === "y" ? "allow_once" : ans === "a" ? "allow_always" : "deny";
      return decision;
    } finally {
      rl.close();
    }
  };
}

async function main(): Promise<number> {
  const opts = parseArgs(process.argv.slice(2));
  if (!opts.prompt) {
    console.error("usage: hx [--root DIR] [--sandbox MODE] [--net deny|allow] [--read-path PATH]... \"your task\"");
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
      ...(makeApprovalHandler() ? { approval: makeApprovalHandler()! } : {}),
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
    engine.close();
  }
}

process.exitCode = await main();
