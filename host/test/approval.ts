// M6 判据：ask 能挂起并恢复；deny 的结果要喂回模型；allow_always 之后不再打扰。
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

// ---------------- 纯函数部分（不需要引擎） ----------------
console.log("规则引擎");
check("通配匹配", wildcardMatch("*rm -rf *", "cd x && rm -rf build"));
check("非匹配不误伤", !wildcardMatch("*rm -rf *", "ls -la"));
check("后匹配者胜", evaluate("bash", "git push", [
  { permission: "bash", pattern: "*", action: "deny" },
  { permission: "bash", pattern: "git*", action: "allow" },
]) === "allow");
check("多主体取最严", evaluateAll("apply_patch", ["a.ts", "b.ts"], [
  { permission: "apply_patch", pattern: "*", action: "allow" },
  { permission: "apply_patch", pattern: "b.ts", action: "deny" },
]) === "deny");
check("无差别 deny 的工具被隐藏",
  !visibleToolNames(["bash", "read"], [{ permission: "bash", pattern: "*", action: "deny" }]).has("bash"));
check("仅部分模式被禁的工具仍可见",
  visibleToolNames(["bash"], [{ permission: "bash", pattern: "*rm -rf*", action: "deny" }]).has("bash"));

// ---------------- 端到端：ask / deny / allow_always ----------------
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

console.log("\n审批回路");
decision = "deny";
const r1 = await agent.run("remove the build dir");

const denied = agent.history.filter((m) => m.role === "tool" && (m.content ?? "").includes("did not approve"));
check("高危命令触发了询问", asked.length >= 1, `asked=${asked.length}`);
check("预览里是命令原文", asked[0]?.preview.includes("rm -rf build") === true, asked[0]?.preview ?? "");
check("拒绝结果被喂回模型", denied.length >= 1, `denied=${denied.length}`);
check("拒绝后循环继续而不是抛异常", r1.stoppedBecause === "final_message", r1.stoppedBecause);
check("安全命令没有被询问", asked.every((a) => a.preview.includes("rm -rf")), asked.map((a) => a.preview).join(" | "));
check("有 approval.requested 事件", events.some((e) => e.type === "approval.requested"));
check("有 approval.resolved 事件", events.some((e) => e.type === "approval.resolved"));

// allow_always：第二次同样的命令不该再问
const asksBefore = asked.length;
decision = "allow_always";
step = 0;
await agent.run("try again, you may remove it");
const newAsks = asked.length - asksBefore;
check("allow_always 之后同样的调用不再询问", newAsks === 1, `本轮询问了 ${newAsks} 次`);
check("会话规则里留下了授权", agent.rules.some((r) => r.action === "allow" && r.pattern === "rm -rf build"));

console.log(`\nfailures=${failures}`);
engine.close();
process.exit(failures === 0 ? 0 : 1);
