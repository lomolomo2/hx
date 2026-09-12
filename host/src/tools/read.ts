import { truncateMiddle } from "../context/truncate.js";
import type { Tool } from "./types.js";

// One file should not consume the whole context window. Past the cap, truncate
// the middle and suggest using offset/limit to read precisely.
const MAX_READ_CHARS = 24_000;

export const readTool: Tool = {
  name: "read",
  description: "Read a text file from the workspace, with line numbers. Use offset/limit for large files.",
  replay: "safe",
  parameters: {
    type: "object",
    properties: {
      path: { type: "string" },
      offset: { type: "integer", description: "First line to return (0-based)." },
      limit: { type: "integer", description: "Maximum number of lines." },
    },
    required: ["path"],
    additionalProperties: false,
  },

  subjects(args) {
    return [String(args["path"] ?? "")];
  },

  async execute(args, ctx) {
    const path = String(args["path"] ?? "");
    const offset = Number(args["offset"] ?? 0);
    const limit = Number(args["limit"] ?? 500);
    const id = ctx.nextItemId();

    try {
      const r = await ctx.engine.fsRead(path, offset, limit);
      ctx.emit({
        type: "item.completed",
        item: { id, type: "file_read", path, lines: r.lines, status: "completed" },
      });
      const numbered = r.content
        .split("\n")
        .map((l, i) => (i === 0 && l === "" ? l : `${offset + i + 1}\t${l}`))
        .join("\n");
      const body =
        numbered.length > MAX_READ_CHARS
          ? `${truncateMiddle(numbered, 8_000, 16_000)}\n(file is large — use offset/limit or grep to read specific parts)`
          : numbered;
      return { content: r.truncated ? `${body}\n… truncated at line ${r.next_offset}` : body };
    } catch (e) {
      ctx.emit({
        type: "item.completed",
        item: { id, type: "file_read", path, lines: 0, status: "failed" },
      });
      return { content: `read failed: ${(e as Error).message}`, isError: true };
    }
  },
};
