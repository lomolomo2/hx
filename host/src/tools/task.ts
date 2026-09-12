import type { Tool } from "./types.js";

// Subagent dispatch.
//
// Three hard rules (see policy/intersect.ts and
// harness-windows-kernel-analogy.md section 2.8):
//   1. Permissions are monotonically non-increasing: a subagent receives the
//      intersection of its parent's permissions and what it asked for
//   2. Context is not shared: a subagent has its own engine process and its own
//      rollout, and only conclusions come back
//   3. What comes back is downgraded to *data*: its output is not an
//      instruction to the parent agent
export const taskTool: Tool = {
  name: "task",
  description:
    "Delegate a self-contained piece of work to a subagent with its own fresh context. " +
    "Use this when a subtask would otherwise flood your context (broad searches, reading many files). " +
    "The subagent cannot have more permissions than you do, and returns only its final report.",
  replay: "never",
  parameters: {
    type: "object",
    properties: {
      name: { type: "string", description: "Short label, e.g. 'explore-parser'." },
      instructions: {
        type: "string",
        description:
          "A complete, self-contained brief. The subagent sees none of your conversation — " +
          "state the goal, the relevant paths, and exactly what to report back.",
      },
      tools: {
        type: "array",
        items: { type: "string" },
        description: "Restrict the subagent to these tools (can only narrow, never widen).",
      },
      sandbox: {
        type: "string",
        enum: ["read-only", "workspace-write"],
        description: "Restrict the subagent's sandbox (can only narrow).",
      },
    },
    required: ["name", "instructions"],
    additionalProperties: false,
  },

  subjects(args) {
    return [String(args["name"] ?? "")];
  },

  preview(args) {
    return `subagent ${String(args["name"] ?? "")}: ${String(args["instructions"] ?? "").slice(0, 200)}`;
  },

  async execute(args, ctx) {
    if (!ctx.spawnSubagent) {
      return { content: "subagents are not available here (depth limit reached)", isError: true };
    }
    const name = String(args["name"] ?? "subagent");
    const instructions = String(args["instructions"] ?? "");
    if (instructions.trim() === "") {
      return { content: "instructions must not be empty", isError: true };
    }

    const req: Parameters<NonNullable<typeof ctx.spawnSubagent>>[0] = { name, instructions };
    if (Array.isArray(args["tools"])) req.tools = (args["tools"] as unknown[]).map(String);
    const sandbox = args["sandbox"];
    if (sandbox === "read-only" || sandbox === "workspace-write") req.sandbox = sandbox;

    try {
      return { content: await ctx.spawnSubagent(req) };
    } catch (e) {
      return { content: `subagent failed: ${(e as Error).message}`, isError: true };
    }
  },
};
