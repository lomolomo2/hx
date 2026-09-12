// hx -- the command-line entry point.
//   tsx src/cli.ts --root /path/to/repo "fix the failing tests"   # one-shot
//   tsx src/cli.ts --root /path/to/repo                           # interactive (REPL)
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
    // Extra read-only grants (repeatable). The default set is narrow: neither
    // $HOME nor /sys/fs/cgroup is in it.
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
      // The event only displays; the actual question lives in the handler, so
      // it is not printed twice
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
 * Interactive approval. Always deny when not a TTY -- allowing something
 * nobody can answer is the same as the rule not existing.
 *
 * ★ In interactive mode it must **reuse** the REPL's readline rather than
 *   opening another: two readlines on one stdin fight over input, and whichever
 *   closes first also swallows the bytes already in the buffer (most visible
 *   when pasting multiple lines).
 *
 *   What gets passed is a box rather than an instance, because the handler has
 *   to be in place when the Agent is constructed while that readline can only
 *   be created once the session is open -- a readline starts consuming stdin
 *   the moment it exists, so creating it early means anything typed in the
 *   meantime has no reader and is simply dropped.
 */
function makeApprovalHandler(box?: { rl?: Readline }): ApprovalHandler | undefined {
  if (!process.stdin.isTTY) return undefined;
  return async (req) => {
    const shared = box?.rl;
    const rl = shared ?? createInterface({ input: process.stdin, output: process.stdout });
    try {
      console.log(`\n  ⚠ approval needed — ${req.tool}`);
      for (const line of req.preview.split("\n").slice(0, 12)) console.log(`    ${line}`);
      const ans = (await rl.question("    [y] allow once  [a] always allow  [n] deny (default n) > ")).trim().toLowerCase();
      const decision: ApprovalDecision = ans === "y" ? "allow_once" : ans === "a" ? "allow_always" : "deny";
      return decision;
    } finally {
      if (!shared) rl.close();
    }
  };
}

const REPL_HELP = [
  "  /help            this help",
  "  /tools           the tools the model can actually see this turn (policy hiding shows up here)",
  "  /sandbox         this session's sandbox reality (enforced / backend / warnings)",
  "  /exit, /quit     quit (Ctrl+D, or Ctrl+C at an empty prompt, do the same)",
  "",
  "  Just type a task and press enter. Ctrl+C mid-run interrupts at a *phase",
  "  boundary*: commands already started are wound up by the engine's timeout",
  "  and exec.kill, so no half-finished side effects are left behind.",
].join("\n");

/**
 * Interactive multi-turn. The same Agent instance is run() repeatedly, keeping
 * both the history and the session.
 *
 * ★ The whole REPL shares **one** readline (the approval question uses it too;
 *   see makeApprovalHandler). This was first written to open and close one per
 *   turn, and the second turn read EOF immediately -- the readline being
 *   closed discarded the buffered input that followed along with it.
 *
 * ★ A line is taken via a persistent 'line' listener plus a queue, not
 *   rl.question() per turn. question() accepts a line only **at the moment it
 *   is called**; anything typed while the model is running is still read by
 *   readline, has no receiver, and is silently dropped. Feeding input through
 *   a pipe is more extreme still: readline consumes the entire pipe in one go,
 *   everything after the first line is lost, and then EOF exits outright.
 *   The queue decouples "reading" from "taking", and gives typeahead for free.
 *   The approval question is not stolen by this listener -- readline does not
 *   emit 'line' while a question is pending.
 *
 * ★ Ctrl+C has two meanings, distinguished by `running`:
 *   at the prompt = quit; mid-task = agent.abort() (which takes effect at a
 *   phase boundary rather than cutting in immediately -- severing a tool
 *   halfway leaves side effects the engine knows nothing about).
 */
async function repl(agent: Agent, root: string, box: { rl?: Readline }): Promise<number> {
  console.log(`\n  workspace ${root}`);
  console.log("  Type a task and press enter. /help for commands.\n");

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
      console.log("\n  ⚠ interrupting — waiting for the current phase to end");
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
        if (!s) console.log("  (no session open)");
        else {
          console.log(`  ${s.sandbox}/${s.net}  enforced=${s.enforced}  net_enforced=${s.netEnforced}  backend=${s.backend}`);
          for (const w of s.warnings) console.log(`  ! ${w}`);
        }
      } else console.log(`  unknown command ${line} (try /help)`);
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
  // No task given: with a terminal, enter interactive mode; without one (a
  // pipe, CI) print usage.
  // ★ The REPL must not run without a TTY -- it would read EOF and exit at
  //   once, which looks like "it did nothing at all".
  const interactive = !opts.prompt;
  if (interactive && !process.stdin.isTTY) {
    console.error("usage: hx [--root DIR] [--sandbox MODE] [--net deny|allow] [--read-path PATH]... \"your task\"");
    console.error("       hx [--root DIR] ...                 # with no task and a terminal, enters interactive mode");
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
        // Must match the server's n_ctx. Set it too high and the server
        // truncates (silently dropping history); too low and compaction kicks
        // in early, throwing information away for nothing. llama.cpp exposes
        // n_ctx at /props.
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
