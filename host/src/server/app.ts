// HTTP + SSE 服务。
//
// ★ 事件直通：对外推送的就是 ThreadEvent 本身，服务层不再造一套 DTO。
//   多一层映射意味着多一处会漂移的真相；前端要什么，就在 ThreadEvent 里加什么。
import { Hono } from "hono";
import { streamSSE } from "hono/streaming";
import { z } from "zod";

import { EngineClient } from "../engine/client.js";
import { Agent } from "../loop/turn.js";
import { parsePreset, preset } from "../policy/presets.js";
import { ToolRegistry, defaultTools } from "../tools/registry.js";
import type { ApprovalDecision, ApprovalRequest } from "../policy/approval.js";
import type { ModelClient } from "../model/types.js";
import type { ThreadEvent } from "../protocol/events.js";

const CreateSession = z.object({
  roots: z.array(z.string()).min(1),
  sandbox: z.enum(["read-only", "workspace-write", "danger-full-access"]).default("workspace-write"),
  net: z.enum(["deny", "allow"]).default("deny"),
  approval: z.string().optional(),
  maxSteps: z.number().int().positive().max(200).optional(),
});

const Prompt = z.object({ text: z.string().min(1) });
const ApprovalReply = z.object({ decision: z.enum(["allow_once", "allow_always", "deny"]) });

interface Session {
  id: string;
  engine: EngineClient;
  agent: Agent;
  events: ThreadEvent[];
  subscribers: Set<(e: ThreadEvent) => void>;
  pending: Map<string, { req: ApprovalRequest; resolve: (d: ApprovalDecision) => void }>;
  busy: boolean;
}

export interface ServerOptions {
  enginePath: string;
  makeModel: () => ModelClient;
}

export function createApp(opts: ServerOptions) {
  const app = new Hono();
  const sessions = new Map<string, Session>();
  let seq = 0;

  const publish = (s: Session, e: ThreadEvent): void => {
    s.events.push(e);
    for (const fn of s.subscribers) fn(e);
  };

  const need = (id: string): Session | undefined => sessions.get(id);

  app.get("/health", (c) => c.json({ ok: true, sessions: sessions.size }));

  app.post("/session", async (c) => {
    const parsed = CreateSession.safeParse(await c.req.json().catch(() => ({})));
    if (!parsed.success) return c.json({ error: parsed.error.issues }, 400);
    const body = parsed.data;

    const id = `s${++seq}`;
    const engine = new EngineClient(opts.enginePath);
    const session: Session = {
      id,
      engine,
      agent: null as unknown as Agent,
      events: [],
      subscribers: new Set(),
      pending: new Map(),
      busy: false,
    };

    const agent = new Agent(engine, opts.makeModel(), new ToolRegistry(defaultTools()), {
      maxSteps: body.maxSteps ?? 20,
      onEvent: (e) => publish(session, e),
      rules: preset(parsePreset(body.approval)),
      enginePath: opts.enginePath,
      // 服务端的 ask 走 HTTP 回调：挂起，等 /approval/:id 回复
      approval: (req) =>
        new Promise<ApprovalDecision>((resolve) => {
          session.pending.set(req.id, { req, resolve });
        }),
    });
    session.agent = agent;
    sessions.set(id, session);

    try {
      await agent.open({ roots: body.roots, sandbox: body.sandbox, net: body.net, name: id });
    } catch (e) {
      engine.close();
      sessions.delete(id);
      return c.json({ error: (e as Error).message }, 400);
    }
    return c.json({ session: id, sandbox: agent.sandbox, tools: agent.availableTools });
  });

  app.get("/session/:id/event", (c) => {
    const s = need(c.req.param("id"));
    if (!s) return c.json({ error: "no such session" }, 404);

    return streamSSE(c, async (stream) => {
      // 先补发已经发生过的事件，客户端晚连也不会漏
      for (const e of s.events) await stream.writeSSE({ data: JSON.stringify(e), event: e.type });

      let alive = true;
      const queue: ThreadEvent[] = [];
      let wake: (() => void) | null = null;
      const sub = (e: ThreadEvent): void => {
        queue.push(e);
        wake?.();
      };
      s.subscribers.add(sub);
      stream.onAbort(() => {
        alive = false;
        s.subscribers.delete(sub);
        wake?.();
      });

      while (alive) {
        while (queue.length > 0) {
          const e = queue.shift()!;
          await stream.writeSSE({ data: JSON.stringify(e), event: e.type });
        }
        await new Promise<void>((r) => {
          wake = r;
          setTimeout(r, 15_000); // 心跳，顺便探活
        });
        if (alive && queue.length === 0) await stream.writeSSE({ data: "", event: "ping" });
      }
      s.subscribers.delete(sub);
    });
  });

  app.post("/session/:id/prompt", async (c) => {
    const s = need(c.req.param("id"));
    if (!s) return c.json({ error: "no such session" }, 404);
    if (s.busy) return c.json({ error: "session is busy" }, 409);

    const parsed = Prompt.safeParse(await c.req.json().catch(() => ({})));
    if (!parsed.success) return c.json({ error: parsed.error.issues }, 400);

    s.busy = true;
    // 异步跑：立刻返回，进展全部走 SSE
    void s.agent
      .run(parsed.data.text)
      .catch((e) => publish(s, { type: "error", message: (e as Error).message }))
      .finally(() => {
        s.busy = false;
      });
    return c.json({ accepted: true }, 202);
  });

  app.post("/session/:id/abort", (c) => {
    const s = need(c.req.param("id"));
    if (!s) return c.json({ error: "no such session" }, 404);
    s.agent.abort();
    return c.json({ aborted: true });
  });

  app.get("/session/:id/approval", (c) => {
    const s = need(c.req.param("id"));
    if (!s) return c.json({ error: "no such session" }, 404);
    return c.json({ pending: [...s.pending.values()].map((p) => p.req) });
  });

  app.post("/session/:id/approval/:aid", async (c) => {
    const s = need(c.req.param("id"));
    if (!s) return c.json({ error: "no such session" }, 404);
    const entry = s.pending.get(c.req.param("aid"));
    if (!entry) return c.json({ error: "no such pending approval" }, 404);

    const parsed = ApprovalReply.safeParse(await c.req.json().catch(() => ({})));
    if (!parsed.success) return c.json({ error: parsed.error.issues }, 400);

    s.pending.delete(c.req.param("aid"));
    entry.resolve(parsed.data.decision);
    return c.json({ ok: true });
  });

  app.get("/session/:id/messages", (c) => {
    const s = need(c.req.param("id"));
    if (!s) return c.json({ error: "no such session" }, 404);
    return c.json({ messages: s.agent.history, events: s.events.length });
  });

  app.delete("/session/:id", (c) => {
    const s = need(c.req.param("id"));
    if (!s) return c.json({ error: "no such session" }, 404);
    s.agent.abort();
    s.engine.close();
    sessions.delete(s.id);
    return c.json({ closed: true });
  });

  return { app, sessions };
}
