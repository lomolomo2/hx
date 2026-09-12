// The single channel of communication with hxd.
//
// ★ This is the only file in the entire host allowed to import
//   node:child_process -- it does exactly one thing: bring up the engine, the
//   "kernel". Every side effect after that goes over the hxp protocol.
//   node:fs or node:child_process appearing in any other file is an
//   architectural back door.
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
        if (!pending) continue; // a late response: the request was abandoned
        this.#pending.delete(msg.reply_to);
        if (msg.ok) pending.resolve(msg.result ?? {});
        else pending.reject(new HxError(msg.error?.code ?? "E_INTERNAL", msg.error?.message ?? "", msg.error?.detail));
      } else {
        hooks.onEvent?.(msg);
      }
    }
  }

  /** Exactly one response per request (protocol section 1). */
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

  // ---- Convenience wrappers. Each corresponds to one op in
  //      proto/hxp-v0.md ----

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
