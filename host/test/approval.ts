// The M6 criterion: ask suspends and resumes; a deny is fed back to the
// model; allow_always stops bothering the user afterwards.
import { EngineClient } from "../src/engine/client.js";
import { Agent } from "../src/loop/turn.js";
import { ScriptedModelClient, callTool, say } from "../src/model/scripted.js";
import { ToolRegistry, defaultTools } from "../src/tools/registry.js";
import { preset } from "../src/policy/presets.js";
import { evaluate, evaluateAll, visibleToolNames, wildcardMatch } from "../src/policy/rules.js";
import type { ApprovalDecision, ApprovalRequest } from "../src/policy/approval.js";
import type { ThreadEvent } from "../src/protocol/events.js";
import type { AssistantTurn, ModelMessage } from "../src/model/types.js";

const ws = process.argv[2];
const hxd = process.argv[3] ?? "../engine/build/hxd";
if (!ws) {
  console.error("usage: tsx test/approval.ts <workspace> [hxd]");
  process.exit(64);
}

let failures = 0;
const check = (name: string, cond: boolean, detail = ""): void => {
  console.log(`  ${cond ? "\x1b[32mPASS\x1b[0m" : "\x1b[31mFAIL\x1b[0m"}  ${name}${!cond && detail ? ` — ${detail}` : ""}`);
  if (!cond) failures++;
};

// ---------------- the pure-function part (no engine needed) ----------------
console.log("rule engine");
check("wildcard matching", wildcardMatch("*rm -rf *", "cd x && rm -rf build"));
check("a non-match does not fire", !wildcardMatch("*rm -rf *", "ls -la"));
check("last match wins", evaluate("bash", "git push", [
  { permission: "bash", pattern: "*", action: "deny" },
  { permission: "bash", pattern: "git*", action: "allow" },
]) === "allow");
check("multiple subjects take the strictest", evaluateAll("apply_patch", ["a.ts", "b.ts"], [
  { permission: "apply_patch", pattern: "*", action: "allow" },
  { permission: "apply_patch", pattern: "b.ts", action: "deny" },
]) === "deny");
check("a blanket-denied tool is hidden",
  !visibleToolNames(["bash", "read"], [{ permission: "bash", pattern: "*", action: "deny" }]).has("bash"));
check("a tool denied for only some patterns stays visible",
  visibleToolNames(["bash"], [{ permission: "bash", pattern: "*rm -rf*", action: "deny" }]).has("bash"));

// ---------------- end to end: ask / deny / allow_always ----------------
const asked: ApprovalRequest[] = [];
let decision: ApprovalDecision = "deny";

let step = 0;
const script = (_m: ModelMessage[]): AssistantTurn => {
  const n = step++;
  if (n === 0) return callTool("c0", "bash", { cmd: "rm -rf build" });
  if (n === 1) return callTool("c1", "bash", { cmd: "rm -rf build" });
  if (n === 2) return callTool("c2", "bash", { cmd: "echo safe-command" });
  return say("done");
};

const events: ThreadEvent[] = [];
const engine = new EngineClient(hxd, { onStderr: (l) => console.error(`[hxd] ${l}`) });
const agent = new Agent(engine, new ScriptedModelClient(script), new ToolRegistry(defaultTools()), {
  maxSteps: 8,
  onEvent: (e) => events.push(e),
  rules: preset("cautious"),
  approval: async (req) => {
    asked.push(req);
    return decision;
  },
});

await agent.open({ roots: [ws], sandbox: "workspace-write", net: "deny", name: "approval" });

console.log("\napproval loop");
decision = "deny";
const r1 = await agent.run("remove the build dir");

const denied = agent.history.filter((m) => m.role === "tool" && (m.content ?? "").includes("did not approve"));
check("a dangerous command triggered a question", asked.length >= 1, `asked=${asked.length}`);
check("the preview holds the command verbatim", asked[0]?.preview.includes("rm -rf build") === true, asked[0]?.preview ?? "");
check("the denial was fed back to the model", denied.length >= 1, `denied=${denied.length}`);
check("after a denial the loop continues rather than throwing", r1.stoppedBecause === "final_message", r1.stoppedBecause);
check("safe commands were not asked about", asked.every((a) => a.preview.includes("rm -rf")), asked.map((a) => a.preview).join(" | "));
check("an approval.requested event was emitted", events.some((e) => e.type === "approval.requested"));
check("an approval.resolved event was emitted", events.some((e) => e.type === "approval.resolved"));

// allow_always: the same command a second time must not ask again
const asksBefore = asked.length;
decision = "allow_always";
step = 0;
await agent.run("try again, you may remove it");
const newAsks = asked.length - asksBefore;
check("after allow_always the same call is not asked again", newAsks === 1, `asked ${newAsks} time(s) this turn`);
check("the grant was left in the session rules", agent.rules.some((r) => r.action === "allow" && r.pattern === "rm -rf build"));

console.log(`\nfailures=${failures}`);
engine.close();
process.exit(failures === 0 ? 0 : 1);
