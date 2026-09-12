// The M4 criterion: when the context overflows it compacts, the compaction
// leaves a trace, and tool calls are never split from their results.
import { EngineClient } from "../src/engine/client.js";
import { Agent } from "../src/loop/turn.js";
import { ScriptedModelClient, callTool, say } from "../src/model/scripted.js";
import { ToolRegistry, defaultTools } from "../src/tools/registry.js";
import { SUMMARY_INSTRUCTION } from "../src/context/compact.js";
import type { ThreadEvent } from "../src/protocol/events.js";
import type { AssistantTurn, ModelMessage } from "../src/model/types.js";
import { fillerCommand } from "./fixtures.js";

const ws = process.argv[2];
const hxd = process.argv[3] ?? "../engine/build/hxd";
if (!ws) { console.error("usage: tsx test/compaction.ts <workspace> [hxd]"); process.exit(64); }

let summarizeCalls = 0;
let toolStep = 0;

// Every step produces a lot of output, pushing toward the context limit
const script = (msgs: ModelMessage[]): AssistantTurn => {
  const last = msgs[msgs.length - 1];
  if (last?.role === "user" && last.content === SUMMARY_INSTRUCTION) {
    summarizeCalls++;
    return { content: "SUMMARY: inspected the repo and ran several commands; next step is to finish.", toolCalls: [], usage: { inputTokens: 0, outputTokens: 0 } };
  }
  if (toolStep < 6) {
    const n = toolStep++;
    return callTool(`c${n}`, "bash", { cmd: fillerCommand(n) });
  }
  return say("Done after several noisy steps.");
};

const events: ThreadEvent[] = [];
const engine = new EngineClient(hxd, { onStderr: (l) => console.error(`[hxd] ${l}`) });
const agent = new Agent(engine, new ScriptedModelClient(script), new ToolRegistry(defaultTools()), {
  maxSteps: 12,
  onEvent: (e) => events.push(e),
  // Deliberately tiny window, to force compaction
  compaction: { enabled: true, contextWindow: 4000, reserveTokens: 500, keepRecentTokens: 1200 },
});

let failures = 0;
const check = (name: string, cond: boolean, detail = ""): void => {
  console.log(`  ${cond ? "\x1b[32mPASS\x1b[0m" : "\x1b[31mFAIL\x1b[0m"}  ${name}${!cond && detail ? ` — ${detail}` : ""}`);
  if (!cond) failures++;
};

await agent.open({ roots: [ws], sandbox: "workspace-write", net: "deny", name: "compact" });
const result = await agent.run("Run a few noisy commands, then finish.");

const compactions = events.filter((e) => e.type === "context.compacted") as Extract<ThreadEvent, { type: "context.compacted" }>[];

console.log();
check("compaction was triggered", compactions.length > 0, `count=${compactions.length}`);
check("tokens went down after compaction", compactions.every((c) => c.tokensAfter < c.tokensBefore),
  compactions.map((c) => `${c.tokensBefore}->${c.tokensAfter}`).join(","));
check("the summarizing model was called", summarizeCalls > 0, `calls=${summarizeCalls}`);
check("the task still wrapped up normally", result.stoppedBecause === "final_message", result.stoppedBecause);

// ★ The key invariant: no orphaned tool message, i.e. a result with no call
const ids = new Set<string>();
let orphans = 0;
for (const m of agent.history) {
  for (const t of m.toolCalls ?? []) ids.add(t.id);
  if (m.role === "tool" && m.toolCallId && !ids.has(m.toolCallId)) orphans++;
}
check("no orphaned tool results", orphans === 0, `orphans=${orphans}`);
check("the original task survived in history", agent.history[0]?.role === "user");
check("a compaction marker appears in history", agent.history.some((m) => (m.content ?? "").includes("<compacted_history>")));

console.log(`\nsteps=${result.steps} compactions=${compactions.length} failures=${failures}`);
engine.close();
process.exit(failures === 0 ? 0 : 1);
