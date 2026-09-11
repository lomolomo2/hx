// 给人看的事件流。与"给模型看的历史"是两条独立的流（规矩一）。
//
// Item 是 UI 的一等公民：先 started、再 updated、最后 completed。
// 这个粒度照抄 Codex SDK —— 前端几乎不用二次加工。

export type ItemStatus = "in_progress" | "completed" | "failed";

export type Item =
  | { id: string; type: "agent_message"; text: string }
  | { id: string; type: "reasoning"; text: string }
  | {
      id: string;
      type: "command_execution";
      command: string;
      output: string;
      exitCode?: number;
      status: ItemStatus;
    }
  | {
      id: string;
      type: "file_change";
      changes: { path: string; kind: "add" | "update" | "delete" }[];
      status: "completed" | "failed";
    }
  | { id: string; type: "file_read"; path: string; lines: number; status: ItemStatus }
  | { id: string; type: "todo_list"; items: { text: string; completed: boolean }[] }
  | {
      id: string;
      type: "subagent";
      name: string;
      instructions: string;
      status: ItemStatus;
      result?: string;
      /** 被驳回的越权诉求 —— 子 agent 被注入的早期信号 */
      rejected?: string[];
    }
  | { id: string; type: "error"; message: string };

export interface Usage {
  inputTokens: number;
  outputTokens: number;
}

export type ThreadEvent =
  | { type: "thread.started"; threadId: string; rollout: string; sandbox: SandboxReport }
  | { type: "turn.started"; turnId: string }
  | { type: "item.started"; item: Item }
  | { type: "item.updated"; item: Item }
  | { type: "item.completed"; item: Item }
  | { type: "turn.completed"; usage: Usage }
  | { type: "turn.failed"; error: { message: string } }
  | {
      type: "context.compacted";
      /** 被摘要替换掉的消息条数 */
      replacedMessages: number;
      tokensBefore: number;
      tokensAfter: number;
    }
  | { type: "approval.requested"; id: string; tool: string; preview: string }
  | { type: "approval.resolved"; id: string; decision: string }
  | { type: "policy.violation"; op: string; reason: string }
  | { type: "error"; message: string };

/** 引擎如实回报的隔离实况——不是我们希望的，是内核真正给的。 */
export interface SandboxReport {
  sandbox: string;
  net: string;
  enforced: boolean;
  netEnforced: boolean;
  landlockAbi: number;
  warnings: string[];
}
