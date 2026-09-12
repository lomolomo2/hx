// The M5 criterion: a multi-step task runs end to end.
//
// It uses the scriptable fake model, so this test is deterministic, offline
// and free -- it verifies that the pipeline is correct, not that the model is
// good.
import { EngineClient } from "../src/engine/client.js";
import { Agent } from "../src/loop/turn.js";
import { ScriptedModelClient, callTool, say } from "../src/model/scripted.js";
import { ToolRegistry, defaultTools } from "../src/tools/registry.js";
import type { ThreadEvent } from "../src/protocol/events.js";
import type { AssistantTurn } from "../src/model/types.js";
import { fixture } from "./fixtures.js";

const ws = process.argv[2];
const hxd = process.argv[3] ?? "../engine/build/hxd";
if (!ws) {
  console.error("usage: tsx test/e2e.ts <workspace> [hxd]");
  process.exit(64);
}


// The model's script: make a plan -> run the tests (fail) -> read the file ->
// apply a patch -> run again (pass) -> wrap up
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
      // The command that runs the tests varies by platform; see the reasoning
      // behind the interpreter choice in test/fixtures.ts.
      return callTool("c1", "bash", { cmd: fixture.runCommand });
    case 2:
      return callTool("c2", "read", { path: "calc.py" });
    case 3:
      return callTool("c3", "apply_patch", { patch: fixture.patch });
    case 4:
      return callTool("c4", "bash", { cmd: fixture.runCommand });
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

// --- assertions ---
const cmds = events.flatMap((e) =>
  e.type === "item.completed" && e.item.type === "command_execution" ? [e.item] : [],
);
const patches = events.flatMap((e) =>
  e.type === "item.completed" && e.item.type === "file_change" ? [e.item] : [],
);

console.log();
check("reached a final message", result.stoppedBecause === "final_message", result.stoppedBecause);
check("the first test run failed", cmds[0]?.exitCode !== 0, `exit=${cmds[0]?.exitCode}`);
check("the patch was written", patches[0]?.status === "completed" && patches[0]?.changes[0]?.path === fixture.sourceName);
check("the tests pass after the fix", cmds[1]?.exitCode === 0, `exit=${cmds[1]?.exitCode} out=${cmds[1]?.output?.slice(0, 80)}`);
check("the sandbox really was enforced", agent.sandbox?.enforced === true);
check("a thread.started event was emitted", events[0]?.type === "thread.started");
check("the loop returned to settling", agent.phase === "settling");

// The file really was changed (read back through the engine; the host never
// touches fs itself)
const after = await engine.fsRead(fixture.sourceName);
check("the file content really became a + b", after.content.includes(fixture.fixedMarker));

// The two streams stay separate: the model's history contains no UI events
const historyRoles = agent.history.map((m) => m.role);
check("history holds all three roles: user/assistant/tool", new Set(historyRoles).size === 3, historyRoles.join(","));

console.log(`\nsteps=${result.steps} failures=${failures}`);
engine.close();
process.exit(failures === 0 ? 0 : 1);
