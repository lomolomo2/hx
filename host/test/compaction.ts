// M4 判据：上下文撑爆时能压缩、压缩留痕、且不切散工具调用与结果。
import { EngineClient } from "../src/engine/client.js";
import { Agent } from "../src/loop/turn.js";
import { ScriptedModelClient, callTool, say } from "../src/model/scripted.js";
import { ToolRegistry, defaultTools } from "../src/tools/registry.js";
import { SUMMARY_INSTRUCTION } from "../src/context/compact.js";
import type { ThreadEvent } from "../src/protocol/events.js";
import type { AssistantTurn, ModelMessage } from "../src/model/types.js";

const ws = process.argv[2];
const hxd = process.argv[3] ?? "../engine/build/hxd";
if (!ws) { console.error("usage: tsx test/compaction.ts <workspace> [hxd]"); process.exit(64); }

let summarizeCalls = 0;
let toolStep = 0;

// 每一步都产出大量输出，逼近上下文上限
const script = (msgs: ModelMessage[]): AssistantTurn => {
  const last = msgs[msgs.length - 1];
  if (last?.role === "user" && last.content === SUMMARY_INSTRUCTION) {
    summarizeCalls++;
    return { content: "SUMMARY: inspected the repo and ran several commands; next step is to finish.", toolCalls: [], usage: { inputTokens: 0, outputTokens: 0 } };
  }
  if (toolStep < 6) {
    const n = toolStep++;
    return callTool(`c${n}`, "bash", { cmd: `python3 -c "print('STEP${n} filler line. ' * 300)"` });
  }
  return say("Done after several noisy steps.");
};

const events: ThreadEvent[] = [];
const engine = new EngineClient(hxd, { onStderr: (l) => console.error(`[hxd] ${l}`) });
const agent = new Agent(engine, new ScriptedModelClient(script), new ToolRegistry(defaultTools()), {
  maxSteps: 12,
  onEvent: (e) => events.push(e),
  // 故意把窗口调得很小，逼出压缩
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
check("触发了压缩", compactions.length > 0, `count=${compactions.length}`);
check("压缩后 token 变少", compactions.every((c) => c.tokensAfter < c.tokensBefore),
  compactions.map((c) => `${c.tokensBefore}->${c.tokensAfter}`).join(","));
check("调用了摘要模型", summarizeCalls > 0, `calls=${summarizeCalls}`);
check("任务仍然正常收尾", result.stoppedBecause === "final_message", result.stoppedBecause);

// ★ 关键不变量：不能留下「有结果没有调用」的孤儿 tool 消息
const ids = new Set<string>();
let orphans = 0;
for (const m of agent.history) {
  for (const t of m.toolCalls ?? []) ids.add(t.id);
  if (m.role === "tool" && m.toolCallId && !ids.has(m.toolCallId)) orphans++;
}
check("没有孤儿 tool 结果", orphans === 0, `orphans=${orphans}`);
check("历史里保留了最初的任务", agent.history[0]?.role === "user");
check("历史里出现了压缩标记", agent.history.some((m) => (m.content ?? "").includes("<compacted_history>")));

console.log(`\nsteps=${result.steps} compactions=${compactions.length} failures=${failures}`);
engine.close();
process.exit(failures === 0 ? 0 : 1);
