// 工具注册表。
//
// ★ 循环里永远不出现任何工具名 —— 只有 registry.get(name).execute(...)。
//   这是 SSDT 分派表的等价物：加工具不需要碰循环。
import type { Tool } from "./types.js";
import { bashTool } from "./bash.js";
import { readTool } from "./read.js";
import { applyPatchTool } from "./apply_patch.js";
import { globTool } from "./glob.js";
import { grepTool } from "./grep.js";
import { taskTool } from "./task.js";
import { todoTool } from "./todo.js";

export class ToolRegistry {
  #tools = new Map<string, Tool>();

  constructor(tools: Tool[]) {
    for (const t of tools) this.#tools.set(t.name, t);
  }

  get(name: string): Tool | undefined {
    return this.#tools.get(name);
  }

  /** 只暴露被允许的工具。被 deny 的工具不该出现在清单里 —— 省 token，
   *  而且模型不会反复尝试一个注定被拒的动作（access mask 模型）。 */
  visible(allowed?: Set<string>): Tool[] {
    const all = [...this.#tools.values()];
    return allowed ? all.filter((t) => allowed.has(t.name)) : all;
  }

  schemas(allowed?: Set<string>): Record<string, unknown>[] {
    return this.visible(allowed).map((t) => ({
      type: "function",
      function: { name: t.name, description: t.description, parameters: t.parameters },
    }));
  }
}

export function defaultTools(): Tool[] {
  return [bashTool, readTool, applyPatchTool, globTool, grepTool, taskTool, todoTool];
}
