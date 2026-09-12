import type { Tool } from "./types.js";

/** Extract the paths a patch touches, for policy matching and the preview. */
function patchPaths(patch: string): string[] {
  const out: string[] = [];
  for (const line of patch.split("\n")) {
    const m = /^\*\*\* (?:Add|Update|Delete) File: (.+)$/.exec(line);
    if (m?.[1]) out.push(m[1].trim());
  }
  return out;
}

export const applyPatchTool: Tool = {
  name: "apply_patch",
  description: `Edit files with a patch. Never rewrite a whole file.

Format:
*** Begin Patch
*** Update File: path/to/file
@@
 context line
-removed line
+added line
*** Add File: path/to/new
+first line
*** Delete File: path/to/old
*** End Patch`,
  replay: "never",
  parameters: {
    type: "object",
    properties: { patch: { type: "string" } },
    required: ["patch"],
    additionalProperties: false,
  },

  subjects(args) {
    return patchPaths(String(args["patch"] ?? ""));
  },

  preview(args) {
    const patch = String(args["patch"] ?? "");
    const paths = patchPaths(patch);
    const stat = patch.split("\n").filter((l) => l.startsWith("+") || l.startsWith("-"));
    const plus = stat.filter((l) => l.startsWith("+")).length;
    const minus = stat.filter((l) => l.startsWith("-")).length;
    return `patch ${paths.join(", ")}  (+${plus} -${minus})`;
  },

  async execute(args, ctx) {
    const patch = String(args["patch"] ?? "");
    const id = ctx.nextItemId();
    try {
      const r = await ctx.engine.applyPatch(patch);
      ctx.emit({
        type: "item.completed",
        item: { id, type: "file_change", changes: r.changes, status: "completed" },
      });
      return { content: r.changes.map((c) => `${c.kind} ${c.path}`).join("\n") || "no changes" };
    } catch (e) {
      ctx.emit({
        type: "item.completed",
        item: { id, type: "file_change", changes: [], status: "failed" },
      });
      return { content: `apply_patch failed: ${(e as Error).message}`, isError: true };
    }
  },
};
