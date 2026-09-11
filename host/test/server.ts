// M8 判据：HTTP + SSE 能驱动完整会话，审批经由 HTTP 挂起与恢复。
import { serve } from "@hono/node-server";

import { createApp } from "../src/server/app.js";
import { ScriptedModelClient, callTool, say } from "../src/model/scripted.js";
import type { AssistantTurn, ModelMessage } from "../src/model/types.js";

const ws = process.argv[2];
const hxd = process.argv[3] ?? "../engine/build/hxd";
if (!ws) {
  console.error("usage: tsx test/server.ts <workspace> [hxd]");
  process.exit(64);
}

let failures = 0;
const check = (name: string, cond: boolean, detail = ""): void => {
  console.log(`  ${cond ? "\x1b[32mPASS\x1b[0m" : "\x1b[31mFAIL\x1b[0m"}  ${name}${!cond && detail ? ` — ${detail}` : ""}`);
  if (!cond) failures++;
};

let step = 0;
const script = (_m: ModelMessage[]): AssistantTurn => {
  const n = step++;
  if (n === 0) return callTool("c0", "bash", { cmd: "echo hello-from-server" });
  if (n === 1) return callTool("c1", "bash", { cmd: "rm -rf build" }); // 触发 ask
  return say("server run complete");
};

const { app } = createApp({ enginePath: hxd, makeModel: () => new ScriptedModelClient(script) });
const port = 4100 + Math.floor(Math.random() * 800);
const server = serve({ fetch: app.fetch, port, hostname: "127.0.0.1" });
const base = `http://127.0.0.1:${port}`;
const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

// ---------- 建会话 ----------
const created = await fetch(`${base}/session`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ roots: [ws], sandbox: "workspace-write", net: "deny", approval: "cautious" }),
});
const session = (await created.json()) as { session: string; sandbox: { enforced: boolean }; tools: string[] };
check("POST /session 成功", created.status === 200, String(created.status));
check("回报了沙箱实况", session.sandbox?.enforced === true);
check("回报了可用工具", Array.isArray(session.tools) && session.tools.includes("bash"), JSON.stringify(session.tools));

// ---------- 订阅 SSE ----------
const seen: string[] = [];
const sse = await fetch(`${base}/session/${session.session}/event`);
check("SSE 建立成功", sse.status === 200 && (sse.headers.get("content-type") ?? "").includes("text/event-stream"),
  sse.headers.get("content-type") ?? "");

void (async () => {
  const reader = sse.body!.getReader();
  const dec = new TextDecoder();
  let buf = "";
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    for (const line of buf.split("\n")) {
      if (line.startsWith("event: ")) seen.push(line.slice(7).trim());
    }
    buf = buf.slice(buf.lastIndexOf("\n") + 1);
  }
})();

// ---------- 发一轮 ----------
const accepted = await fetch(`${base}/session/${session.session}/prompt`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ text: "say hello then clean the build dir" }),
});
check("POST /prompt 立刻返回 202（异步）", accepted.status === 202, String(accepted.status));

// ---------- 等待挂起的审批 ----------
let pending: { pending: { id: string; tool: string; preview: string }[] } = { pending: [] };
for (let i = 0; i < 50 && pending.pending.length === 0; i++) {
  await sleep(100);
  pending = (await (await fetch(`${base}/session/${session.session}/approval`)).json()) as typeof pending;
}
check("高危调用在服务端挂起等待审批", pending.pending.length === 1, JSON.stringify(pending.pending));
check("待审批项带预览", pending.pending[0]?.preview.includes("rm -rf build") === true);

// ---------- 回复审批 ----------
const replied = await fetch(`${base}/session/${session.session}/approval/${pending.pending[0]!.id}`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ decision: "deny" }),
});
check("POST /approval 回复成功", replied.status === 200, String(replied.status));

// ---------- 等待收尾 ----------
for (let i = 0; i < 60; i++) {
  await sleep(100);
  if (seen.includes("turn.completed")) break;
}
check("SSE 收到 thread.started", seen.includes("thread.started"), seen.join(","));
check("SSE 收到 item 事件", seen.some((e) => e.startsWith("item.")), seen.join(","));
check("SSE 收到 approval.requested", seen.includes("approval.requested"), seen.join(","));
check("SSE 收到 turn.completed", seen.includes("turn.completed"), seen.join(","));

const msgs = (await (await fetch(`${base}/session/${session.session}/messages`)).json()) as {
  messages: { role: string; content: string | null }[];
};
check("GET /messages 返回历史", msgs.messages.length > 3, String(msgs.messages.length));
check("拒绝结果进了历史", msgs.messages.some((m) => (m.content ?? "").includes("did not approve")));

const closed = await fetch(`${base}/session/${session.session}`, { method: "DELETE" });
check("DELETE /session 关闭会话", closed.status === 200, String(closed.status));

console.log(`\nfailures=${failures}`);
server.close();
process.exit(failures === 0 ? 0 : 1);
