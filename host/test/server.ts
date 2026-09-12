// The M8 criterion: HTTP + SSE can drive a complete session, with approval
// suspending and resuming over HTTP.
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
  if (n === 1) return callTool("c1", "bash", { cmd: "rm -rf build" }); // triggers ask
  return say("server run complete");
};

const { app } = createApp({ enginePath: hxd, makeModel: () => new ScriptedModelClient(script) });
const port = 4100 + Math.floor(Math.random() * 800);
const server = serve({ fetch: app.fetch, port, hostname: "127.0.0.1" });
const base = `http://127.0.0.1:${port}`;
const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

// ---------- create the session ----------
const created = await fetch(`${base}/session`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ roots: [ws], sandbox: "workspace-write", net: "deny", approval: "cautious" }),
});
const session = (await created.json()) as { session: string; sandbox: { enforced: boolean }; tools: string[] };
check("POST /session succeeded", created.status === 200, String(created.status));
check("the sandbox reality was reported", session.sandbox?.enforced === true);
check("the available tools were reported", Array.isArray(session.tools) && session.tools.includes("bash"), JSON.stringify(session.tools));

// ---------- subscribe to SSE ----------
const seen: string[] = [];
const sse = await fetch(`${base}/session/${session.session}/event`);
check("SSE was established", sse.status === 200 && (sse.headers.get("content-type") ?? "").includes("text/event-stream"),
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

// ---------- send one turn ----------
const accepted = await fetch(`${base}/session/${session.session}/prompt`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ text: "say hello then clean the build dir" }),
});
check("POST /prompt returns 202 immediately (async)", accepted.status === 202, String(accepted.status));

// ---------- wait for the suspended approval ----------
let pending: { pending: { id: string; tool: string; preview: string }[] } = { pending: [] };
for (let i = 0; i < 50 && pending.pending.length === 0; i++) {
  await sleep(100);
  pending = (await (await fetch(`${base}/session/${session.session}/approval`)).json()) as typeof pending;
}
check("the dangerous call suspended server-side awaiting approval", pending.pending.length === 1, JSON.stringify(pending.pending));
check("the pending item carries a preview", pending.pending[0]?.preview.includes("rm -rf build") === true);

// ---------- answer the approval ----------
const replied = await fetch(`${base}/session/${session.session}/approval/${pending.pending[0]!.id}`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ decision: "deny" }),
});
check("POST /approval replied successfully", replied.status === 200, String(replied.status));

// ---------- wait for the wrap-up ----------
for (let i = 0; i < 60; i++) {
  await sleep(100);
  if (seen.includes("turn.completed")) break;
}
check("SSE received thread.started", seen.includes("thread.started"), seen.join(","));
check("SSE received item events", seen.some((e) => e.startsWith("item.")), seen.join(","));
check("SSE received approval.requested", seen.includes("approval.requested"), seen.join(","));
check("SSE received turn.completed", seen.includes("turn.completed"), seen.join(","));

const msgs = (await (await fetch(`${base}/session/${session.session}/messages`)).json()) as {
  messages: { role: string; content: string | null }[];
};
check("GET /messages returns the history", msgs.messages.length > 3, String(msgs.messages.length));
check("the denial made it into the history", msgs.messages.some((m) => (m.content ?? "").includes("did not approve")));

const closed = await fetch(`${base}/session/${session.session}`, { method: "DELETE" });
check("DELETE /session closes the session", closed.status === 200, String(closed.status));

console.log(`\nfailures=${failures}`);
server.close();
process.exit(failures === 0 ? 0 : 1);
