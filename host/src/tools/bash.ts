import { truncateMiddle } from "../context/truncate.js";
import { shellCommand, shellName } from "../platform.js";
import type { Tool } from "./types.js";

// Feeding 400,000 characters of output to the model is pointless and merely
// burns the context. Keep 4k of the head (what ran, the first error) and 12k
// of the tail (the final state, the test verdict, the stack trace).
const MAX_OUTPUT = 120_000; // collection cap
const HEAD_CHARS = 4_000;
const TAIL_CHARS = 12_000;
const DEFAULT_TIMEOUT_MS = 120_000;

// ★ The tool name is fixed as "bash" (policy rules, presets and approval all
//   match against it), but **the description must say who actually runs the
//   command**. Call it bash, run cmd.exe underneath, and do not tell the
//   model, and it will send POSIX dialect throughout and fail on every line --
//   with cmd's error messages, from which the model cannot tell that the
//   problem is the dialect.
//   Set HX_SHELL for a different shell (Git Bash's bash.exe, say).
const SHELL_OVERRIDE = process.env["HX_SHELL"];

export const bashTool: Tool = {
  name: "bash",
  description:
    `Run a command with ${shellName(SHELL_OVERRIDE)} inside the sandboxed workspace. ` +
    `Write commands in ${shellName(SHELL_OVERRIDE)} syntax. Output is truncated. ` +
    "The command runs under OS-level isolation: paths outside the workspace are not readable.",
  replay: "never",
  parameters: {
    type: "object",
    properties: {
      cmd: { type: "string", description: "The shell command to run." },
      timeout_ms: { type: "integer", description: "Kill the command after this many ms." },
    },
    required: ["cmd"],
    additionalProperties: false,
  },

  subjects(args) {
    return [String(args["cmd"] ?? "")];
  },

  preview(args) {
    return `$ ${String(args["cmd"] ?? "")}`;
  },

  async execute(args, ctx) {
    const cmd = String(args["cmd"] ?? "");
    const timeoutMs = Number(args["timeout_ms"] ?? DEFAULT_TIMEOUT_MS);
    const id = ctx.nextItemId();

    ctx.emit({
      type: "item.started",
      item: { id, type: "command_execution", command: cmd, output: "", status: "in_progress" },
    });

    // platform decides what argv this becomes (bash -c / cmd /c / pwsh
    // -Command). Use -c rather than -lc: a login shell reads ~/.profile, and
    // $HOME is unreadable inside the sandbox, so every command would carry a
    // line of "Permission denied" noise. We construct the environment
    // explicitly anyway.
    const { cell } = await ctx.engine.execStart(shellCommand(cmd, SHELL_OVERRIDE), { timeoutMs });

    let output = "";
    let exitCode: number | null = null;
    let truncated = false;

    for (;;) {
      const r = await ctx.engine.execWait(cell, 1000, 32768);
      if (r.data) {
        output += r.data;
        if (output.length > MAX_OUTPUT) {
          output = output.slice(0, MAX_OUTPUT);
          truncated = true;
          await ctx.engine.execKill(cell, "KILL");
        }
        ctx.emit({
          type: "item.updated",
          item: { id, type: "command_execution", command: cmd, output, status: "in_progress" },
        });
      }
      if (r.done) {
        exitCode = r.exit_code;
        if (r.timed_out) truncated = true;
        break;
      }
      if (truncated) break;
    }

    ctx.emit({
      type: "item.completed",
      item: {
        id,
        type: "command_execution",
        command: cmd,
        output,
        ...(exitCode !== null ? { exitCode } : {}),
        status: exitCode === 0 ? "completed" : "failed",
      },
    });

    // ★ A failure is fed back to the model too, never thrown -- otherwise the
    //   model never learns that this route does not work
    const header = `exit_code=${exitCode ?? "unknown"}${truncated ? " (output truncated)" : ""}`;
    return {
      content: `${header}\n${truncateMiddle(output, HEAD_CHARS, TAIL_CHARS)}`,
      isError: exitCode !== 0,
    };
  },
};
