import type { Tool } from "./types.js";

export const globTool: Tool = {
  name: "glob",
  description: "Find files by glob pattern, e.g. 'src/**/*.ts'. '**' matches zero or more directories.",
  replay: "safe",
  parameters: {
    type: "object",
    properties: { pattern: { type: "string" }, limit: { type: "integer" } },
    required: ["pattern"],
    additionalProperties: false,
  },

  subjects(args) {
    return [String(args["pattern"] ?? "")];
  },

  async execute(args, ctx) {
    const pattern = String(args["pattern"] ?? "");
    try {
      const r = await ctx.engine.glob(pattern, Number(args["limit"] ?? 500));
      const list = r.matches.join("\n");
      return { content: r.truncated ? `${list}\n… truncated` : list || "(no matches)" };
    } catch (e) {
      return { content: `glob failed: ${(e as Error).message}`, isError: true };
    }
  },
};
