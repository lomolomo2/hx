import type { EngineClient } from "../engine/client.js";
import type { ThreadEvent } from "../protocol/events.js";

export interface SubagentRequest {
  name: string;
  instructions: string;
  /** Can only narrow, never widen -- see policy/intersect.ts */
  tools?: string[];
  sandbox?: "read-only" | "workspace-write";
}

export interface ToolContext {
  engine: EngineClient;
  emit: (e: ThreadEvent) => void;
  nextItemId: () => string;
  todos: { text: string; completed: boolean }[];
  /** Injected by the Agent. Its absence means the depth cap has been reached. */
  spawnSubagent?: (req: SubagentRequest) => Promise<string>;
}

export interface ToolResult {
  content: string;
  isError?: boolean;
}

export interface Tool {
  name: string;
  description: string;
  parameters: Record<string, unknown>;
  /** Crash-recovery semantics, passed through to the engine (see
   *  proto/hxp-v0.md section 1) */
  replay: "never" | "safe";
  /** The subjects policy matching runs against. For bash it is the command
   *  line; for file tools, the path. Not implementing it means no subjects. */
  subjects?(args: Record<string, unknown>): string[];
  /** The approval preview shown to a human. Without it, falls back to joining
   *  subjects. */
  preview?(args: Record<string, unknown>): string;
  execute(args: Record<string, unknown>, ctx: ToolContext): Promise<ToolResult>;
}
