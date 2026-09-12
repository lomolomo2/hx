import { truncateMiddle } from "../context/truncate.js";
import { shellCommand, shellName } from "../platform.js";
import type { Tool } from "./types.js";

// 40 万字符的输出喂给模型没有意义，只会烧光上下文。
// 头留 4k（跑了什么、最初的错误），尾留 12k（最终状态、测试结论、堆栈）。
const MAX_OUTPUT = 120_000; // 采集上限
const HEAD_CHARS = 4_000;
const TAIL_CHARS = 12_000;
const DEFAULT_TIMEOUT_MS = 120_000;

// ★ 工具名固定是 "bash"（策略规则、presets、审批都按它匹配），
//   但**描述里必须写清真正跑命令的是谁**。名字叫 bash、底下是 cmd.exe
//   而不告诉模型，它就会一路发 POSIX 方言，然后每条都失败 ——
//   而且失败信息是 cmd 的，模型根本看不出问题出在方言上。
//   需要换 shell 就设 HX_SHELL（例如 Git Bash 的 bash.exe）。
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

    // 具体包成什么 argv 由 platform 决定（bash -c / cmd /c / pwsh -Command）。
    // 用 -c 而不是 -lc：登录 shell 会去读 ~/.profile，而 $HOME 在沙箱里不可读，
    // 于是每条命令都会带一行 "Permission denied" 噪音。环境我们已显式构造好了。
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

    // ★ 失败也要把结果喂回模型，不能抛异常 —— 否则模型学不会"此路不通"
    const header = `exit_code=${exitCode ?? "unknown"}${truncated ? " (output truncated)" : ""}`;
    return {
      content: `${header}\n${truncateMiddle(output, HEAD_CHARS, TAIL_CHARS)}`,
      isError: exitCode !== 0,
    };
  },
};
