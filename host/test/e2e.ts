// M5 判据：端到端跑通一个多步任务。
//
// 用可脚本化的假模型，所以这个测试是确定的、离线的、免费的 ——
// 它验的是"管道对不对"，不是"模型好不好"。
import { EngineClient } from "../src/engine/client.js";
import { Agent } from "../src/loop/turn.js";
import { ScriptedModelClient, callTool, say } from "../src/model/scripted.js";
import { ToolRegistry, defaultTools } from "../src/tools/registry.js";
import type { ThreadEvent } from "../src/protocol/events.js";
import type { AssistantTurn } from "../src/model/types.js";

const ws = process.argv[2];
const hxd = process.argv[3] ?? "../engine/build/hxd";
if (!ws) {
  console.error("usage: tsx test/e2e.ts <workspace> [hxd]");
  process.exit(64);
}

const PATCH = `*** Begin Patch
*** Update File: calc.py
@@
 def add(a, b):
-    return a - b
+    return a + b
*** End Patch
`;

// 模型的剧本：立计划 → 跑测试(失败) → 读文件 → 打补丁 → 再跑(通过) → 收尾
const script = (_msgs: unknown, step: number): AssistantTurn => {
  switch (step) {
    case 0:
      return callTool("c0", "todo", {
        items: [
          { text: "run the failing test", completed: false },
          { text: "fix calc.py", completed: false },
        ],
      });
    case 1:
      // -B：不写 .pyc。补丁把 "a - b" 改成 "a + b" 字节数不变，若又落在同一秒内，
      // Python 的 (mtime秒, size) 校验会判定缓存有效 —— 改对了却仍报失败。
      return callTool("c1", "bash", { cmd: "python3 -B test_calc.py" });
    case 2:
      return callTool("c2", "read", { path: "calc.py" });
    case 3:
      return callTool("c3", "apply_patch", { patch: PATCH });
    case 4:
      return callTool("c4", "bash", { cmd: "python3 -B test_calc.py" });
    case 5:
      return callTool("c5", "todo", {
        items: [
          { text: "run the failing test", completed: true },
          { text: "fix calc.py", completed: true },
        ],
      });
    default:
      return say("Fixed: add() was subtracting. Tests pass now.");
  }
};

const events: ThreadEvent[] = [];
const engine = new EngineClient(hxd, { onStderr: (l) => console.error(`[hxd] ${l}`) });
const model = new ScriptedModelClient(script);
const agent = new Agent(engine, model, new ToolRegistry(defaultTools()), {
  maxSteps: 12,
  onEvent: (e) => events.push(e),
});

let failures = 0;
const check = (name: string, cond: boolean, detail = ""): void => {
  console.log(`  ${cond ? "\x1b[32mPASS\x1b[0m" : "\x1b[31mFAIL\x1b[0m"}  ${name}${detail && !cond ? ` — ${detail}` : ""}`);
  if (!cond) failures++;
};

await agent.open({ roots: [ws], sandbox: "workspace-write", net: "deny", name: "e2e" });
const result = await agent.run("The add() function is broken. Find it, fix it, and verify.");

// --- 断言 ---
const cmds = events.flatMap((e) =>
  e.type === "item.completed" && e.item.type === "command_execution" ? [e.item] : [],
);
const patches = events.flatMap((e) =>
  e.type === "item.completed" && e.item.type === "file_change" ? [e.item] : [],
);

console.log();
check("走到了最终回复", result.stoppedBecause === "final_message", result.stoppedBecause);
check("第一次跑测试失败", cmds[0]?.exitCode !== 0, `exit=${cmds[0]?.exitCode}`);
check("补丁落盘成功", patches[0]?.status === "completed" && patches[0]?.changes[0]?.path === "calc.py");
check("修完后测试通过", cmds[1]?.exitCode === 0, `exit=${cmds[1]?.exitCode} out=${cmds[1]?.output?.slice(0, 80)}`);
check("沙箱确实生效", agent.sandbox?.enforced === true);
check("有 thread.started 事件", events[0]?.type === "thread.started");
check("循环结束时回到 settling", agent.phase === "settling");

// 文件真的被改了（经引擎读回，宿主自己不碰 fs）
const after = await engine.fsRead("calc.py");
check("文件内容确实变成了 a + b", after.content.includes("return a + b"));

// 两条流分离：给模型的历史里不含 UI 事件
const historyRoles = agent.history.map((m) => m.role);
check("历史里有 user/assistant/tool 三种角色", new Set(historyRoles).size === 3, historyRoles.join(","));

console.log(`\nsteps=${result.steps} failures=${failures}`);
engine.close();
process.exit(failures === 0 ? 0 : 1);
