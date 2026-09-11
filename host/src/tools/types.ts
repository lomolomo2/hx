import type { EngineClient } from "../engine/client.js";
import type { ThreadEvent } from "../protocol/events.js";

export interface SubagentRequest {
  name: string;
  instructions: string;
  /** 只能收窄，不能放宽 —— 见 policy/intersect.ts */
  tools?: string[];
  sandbox?: "read-only" | "workspace-write";
}

export interface ToolContext {
  engine: EngineClient;
  emit: (e: ThreadEvent) => void;
  nextItemId: () => string;
  todos: { text: string; completed: boolean }[];
  /** 由 Agent 注入。不可用时表示已达深度上限。 */
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
  /** 崩溃恢复语义，透传给引擎（见 proto/hxp-v0.md §1） */
  replay: "never" | "safe";
  /** 参与策略匹配的主体。bash 是命令行，文件类工具是路径。不实现即视为无主体。 */
  subjects?(args: Record<string, unknown>): string[];
  /** 给人看的审批预览。不实现则回退到 subjects 的拼接。 */
  preview?(args: Record<string, unknown>): string;
  execute(args: Record<string, unknown>, ctx: ToolContext): Promise<ToolResult>;
}
