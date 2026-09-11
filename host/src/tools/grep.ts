import { truncateMiddle } from "../context/truncate.js";
import type { Tool } from "./types.js";

// ★ 直接把 argv 数组交给引擎执行，不经过 shell。
//   模型给的 pattern 里就算有 $(...) 或 ; rm -rf 也只是一个普通的正则字符串。
//   如果这里拼成 `bash -c "grep ${pattern}"`，那就是一个注入口子。
const SKIP_DIRS = ["node_modules", ".git", "build", "dist", "__pycache__", ".venv", "vendor"];

export const grepTool: Tool = {
  name: "grep",
  description:
    "Search file contents with a regular expression. Faster and cheaper than shelling out. " +
    "Returns matching lines as path:line:text.",
  replay: "safe",
  parameters: {
    type: "object",
    properties: {
      pattern: { type: "string", description: "POSIX extended regular expression." },
      path: { type: "string", description: "File or directory to search. Defaults to the workspace root." },
      include: { type: "string", description: "Only search files matching this glob, e.g. '*.ts'." },
      ignore_case: { type: "boolean" },
      max_results: { type: "integer", description: "Default 200." },
    },
    required: ["pattern"],
    additionalProperties: false,
  },

  subjects(args) {
    return [String(args["pattern"] ?? "")];
  },

  async execute(args, ctx) {
    const pattern = String(args["pattern"] ?? "");
    if (pattern === "") return { content: "pattern is required", isError: true };

    const maxResults = Number(args["max_results"] ?? 200);
    const argv = ["grep", "-rnI", "--color=never"];
    for (const d of SKIP_DIRS) argv.push(`--exclude-dir=${d}`);
    if (args["ignore_case"]) argv.push("-i");
    if (args["include"]) argv.push(`--include=${String(args["include"])}`);
    argv.push("-E", "-e", pattern, String(args["path"] ?? "."));

    try {
      const { cell } = await ctx.engine.execStart(argv, { timeoutMs: 30_000 });
      let out = "";
      for (;;) {
        const r = await ctx.engine.execWait(cell, 1000, 65536);
        out += r.data;
        if (r.done) {
          // grep 的退出码 1 表示"没有匹配"，不是错误
          if (r.exit_code !== 0 && r.exit_code !== 1 && out.trim() === "") {
            return { content: `grep exited ${r.exit_code}`, isError: true };
          }
          break;
        }
      }

      const lines = out.split("\n").filter((l) => l !== "");
      if (lines.length === 0) return { content: "(no matches)" };
      const shown = lines.slice(0, maxResults);
      const suffix = lines.length > maxResults ? `\n… ${lines.length - maxResults} more matches` : "";
      return { content: truncateMiddle(shown.join("\n"), 4000, 4000) + suffix };
    } catch (e) {
      return { content: `grep failed: ${(e as Error).message}`, isError: true };
    }
  },
};
