// 与 hxd 通信的唯一通道。
//
// ★ 本文件是整个 host 里唯一允许 import node:child_process 的地方 ——
//   它要做的事只有一件：把引擎这个"内核"拉起来。此后所有副作用都走 hxp 协议。
//   其余任何文件出现 node:fs / node:child_process，都是在架构上开后门。
/* eslint-disable no-restricted-imports */
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
/* eslint-enable no-restricted-imports */

import { HxError, isReply, type HxEvent, type HxMessage, type HxRequest } from "../protocol/hxp.js";

type Pending = {
  resolve: (result: Record<string, unknown>) => void;
  reject: (err: Error) => void;
};

export interface EngineEvents {
  onEvent?: (e: HxEvent) => void;
  onStderr?: (line: string) => void;
}

export class EngineClient {
  #proc: ChildProcessWithoutNullStreams;
  #pending = new Map<string, Pending>();
  #buf = "";
  #seq = 0;
  #closed = false;

  constructor(hxdPath: string, hooks: EngineEvents = {}) {
    this.#proc = spawn(hxdPath, [], { stdio: ["pipe", "pipe", "pipe"] });
    this.#proc.stdout.setEncoding("utf8");
    this.#proc.stdout.on("data", (chunk: string) => this.#onStdout(chunk, hooks));
    this.#proc.stderr.setEncoding("utf8");
    this.#proc.stderr.on("data", (chunk: string) => {
      for (const line of chunk.split("\n")) if (line) hooks.onStderr?.(line);
    });
    this.#proc.on("exit", (code, signal) => {
      this.#closed = true;
      const err = new Error(`hxd exited (code=${code} signal=${signal})`);
      for (const [, p] of this.#pending) p.reject(err);
      this.#pending.clear();
    });
  }

  #onStdout(chunk: string, hooks: EngineEvents): void {
    this.#buf += chunk;
    let nl: number;
    while ((nl = this.#buf.indexOf("\n")) >= 0) {
      const line = this.#buf.slice(0, nl);
      this.#buf = this.#buf.slice(nl + 1);
      if (!line) continue;

      let msg: HxMessage;
      try {
        msg = JSON.parse(line) as HxMessage;
      } catch {
        hooks.onStderr?.(`unparsable line from hxd: ${line.slice(0, 200)}`);
        continue;
      }

      if (isReply(msg)) {
        const pending = this.#pending.get(msg.reply_to);
        if (!pending) continue; // 迟到的响应：请求已被放弃
        this.#pending.delete(msg.reply_to);
        if (msg.ok) pending.resolve(msg.result ?? {});
        else pending.reject(new HxError(msg.error?.code ?? "E_INTERNAL", msg.error?.message ?? "", msg.error?.detail));
      } else {
        hooks.onEvent?.(msg);
      }
    }
  }

  /** 每个请求恰好等到一个响应（协议 §1）。 */
  call(op: string, args?: Record<string, unknown>, replay: "never" | "safe" = "never"): Promise<Record<string, unknown>> {
    if (this.#closed) return Promise.reject(new Error("engine is closed"));
    const req: HxRequest = { id: `r${++this.#seq}`, op, replay };
    if (args) req.args = args;
    return new Promise((resolve, reject) => {
      this.#pending.set(req.id, { resolve, reject });
      this.#proc.stdin.write(JSON.stringify(req) + "\n", (err) => {
        if (err) {
          this.#pending.delete(req.id);
          reject(err);
        }
      });
    });
  }

  close(): void {
    if (this.#closed) return;
    this.#proc.stdin.end();
  }

  // ---- 便捷封装。每一个都对应 proto/hxp-v0.md 里的一个 op ----

  selfTest() {
    return this.call("selftest", undefined, "safe");
  }

  sessionOpen(opts: {
    roots: string[];
    sandbox: string;
    net: string;
    name?: string;
    extraReadPaths?: string[];
  }) {
    const args: Record<string, unknown> = { roots: opts.roots, sandbox: opts.sandbox, net: opts.net };
    if (opts.name) args["name"] = opts.name;
    if (opts.extraReadPaths?.length) args["extra_read_paths"] = opts.extraReadPaths;
    return this.call("session.open", args);
  }

  execStart(cmd: string[], opts: { cwd?: string; timeoutMs?: number } = {}) {
    const args: Record<string, unknown> = { cmd };
    if (opts.cwd) args["cwd"] = opts.cwd;
    if (opts.timeoutMs) args["timeout_ms"] = opts.timeoutMs;
    return this.call("exec.start", args) as Promise<{ cell: string }>;
  }

  execWait(cell: string, yieldMs = 1000, maxBytes = 65536) {
    return this.call("exec.wait", { cell, yield_ms: yieldMs, max_bytes: maxBytes }, "safe") as Promise<{
      done: boolean;
      data: string;
      truncated: boolean;
      exit_code: number | null;
      dropped_bytes: number;
      timed_out?: boolean;
    }>;
  }

  execKill(cell: string, signal = "TERM") {
    return this.call("exec.kill", { cell, signal });
  }

  fsRead(path: string, offset = 0, limit = 2000) {
    return this.call("fs.read", { path, offset, limit }, "safe") as Promise<{
      content: string;
      lines: number;
      next_offset: number;
      truncated: boolean;
    }>;
  }

  applyPatch(patch: string) {
    return this.call("fs.apply_patch", { patch }) as Promise<{
      changes: { path: string; kind: "add" | "update" | "delete" }[];
    }>;
  }

  glob(pattern: string, limit = 500) {
    return this.call("fs.glob", { pattern, limit }, "safe") as Promise<{ matches: string[]; truncated: boolean }>;
  }

  logAppend(record: Record<string, unknown>) {
    return this.call("log.append", { record });
  }
}
