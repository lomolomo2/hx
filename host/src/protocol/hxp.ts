// hxp v0 的宿主侧类型。规范：proto/hxp-v0.md（引擎与宿主都以它为准）

export type Replay = "never" | "safe";

export interface HxRequest {
  id: string;
  op: string;
  args?: Record<string, unknown>;
  replay?: Replay;
}

export interface HxReply {
  reply_to: string;
  ok: boolean;
  result?: Record<string, unknown>;
  error?: { code: string; message: string; detail?: unknown };
}

export interface HxEvent {
  event: string;
  [k: string]: unknown;
}

export type HxMessage = HxReply | HxEvent;

export function isReply(m: HxMessage): m is HxReply {
  return typeof (m as HxReply).reply_to === "string";
}

export class HxError extends Error {
  constructor(
    readonly code: string,
    message: string,
    readonly detail?: unknown,
  ) {
    super(`${code}: ${message}`);
    this.name = "HxError";
  }
}
