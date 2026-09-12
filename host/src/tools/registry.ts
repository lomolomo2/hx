// The tool registry.
//
// ★ No tool name ever appears in the loop -- only
//   registry.get(name).execute(...). This is the equivalent of the SSDT
//   dispatch table: adding a tool requires no change to the loop.
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

  /** Expose only permitted tools. A denied tool should not appear in the list
   *  at all -- it saves tokens, and the model stops repeatedly attempting an
   *  action destined to be refused (the access-mask model). */
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
