// The event stream for people. Separate from "the history for the model" --
// two independent streams (rule one).
//
// Item is a first-class citizen of the UI: started, then updated, then
// completed. The granularity is copied from the Codex SDK -- a front end needs
// almost no further processing.

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
      /** Rejected over-reaching requests -- an early signal that a subagent
       *  has been injected */
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
      /** How many messages the summary replaced */
      replacedMessages: number;
      tokensBefore: number;
      tokensAfter: number;
    }
  | { type: "approval.requested"; id: string; tool: string; preview: string }
  | { type: "approval.resolved"; id: string; decision: string }
  | { type: "policy.violation"; op: string; reason: string }
  | { type: "error"; message: string };

/** The isolation reality the engine reports honestly -- not what we hoped for,
 *  but what the kernel actually granted. */
export interface SandboxReport {
  sandbox: string;
  net: string;
  enforced: boolean;
  netEnforced: boolean;
  /**
   * What is doing the enforcing on the kernel side: "landlock" /
   * "appcontainer" / "none".
   *
   * ★ This used to be landlockAbi: number, which welded a platform-specific
   *   implementation detail into the protocol -- what the host has always
   *   needed to decide is "was there a real sandbox this time", not "which
   *   kernel interface, at which version". For the details, the rollout's
   *   first session_meta record carries the full caps.
   */
  backend: string;
  warnings: string[];
}
