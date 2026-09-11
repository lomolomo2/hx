import type { Tool } from "./types.js";

// 计划是工具，不是提示词里的自觉。
// 实测 codex 的 update_plan 在 28 个会话里被调用了 79 次。
export const todoTool: Tool = {
  name: "todo",
  description: "Record or update your plan. Call this when the task has multiple steps, and again as steps complete.",
  replay: "safe",
  parameters: {
    type: "object",
    properties: {
      items: {
        type: "array",
        items: {
          type: "object",
          properties: { text: { type: "string" }, completed: { type: "boolean" } },
          required: ["text", "completed"],
          additionalProperties: false,
        },
      },
    },
    required: ["items"],
    additionalProperties: false,
  },

  async execute(args, ctx) {
    const raw = Array.isArray(args["items"]) ? (args["items"] as { text: string; completed: boolean }[]) : [];
    ctx.todos.length = 0;
    ctx.todos.push(...raw.map((i) => ({ text: String(i.text), completed: Boolean(i.completed) })));
    ctx.emit({ type: "item.completed", item: { id: ctx.nextItemId(), type: "todo_list", items: [...ctx.todos] } });
    const done = ctx.todos.filter((t) => t.completed).length;
    return { content: `plan updated (${done}/${ctx.todos.length} done)` };
  },
};
