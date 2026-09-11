import type { Rule } from "./rules.js";

// 高危命令：不是"禁止"，而是"值得停下来问一句"。
// 挑选标准：不可逆、影响工作区之外、或会对外发布。
const DANGEROUS_BASH = [
  "*rm -rf *",
  "*rm -fr *",
  "*git push*",
  "*git reset --hard*",
  "*git clean*",
  "*npm publish*",
  "*npm install*",
  "*pip install*",
  "*curl *",
  "*wget *",
  "*chmod 777*",
  "*mkfs*",
  "*dd if=*",
  "*shutdown*",
  "*reboot*",
];

export type PresetName = "auto" | "cautious" | "strict";

export function preset(name: PresetName): Rule[] {
  switch (name) {
    case "auto":
      // 全自动：一切放行。沙箱仍然在，只是不再就意图发问。
      return [];

    case "cautious":
      // 默认推荐：日常操作放行，高危命令问一句。
      return DANGEROUS_BASH.map((pattern) => ({
        permission: "bash",
        pattern,
        action: "ask" as const,
      }));

    case "strict":
      // 任何有副作用的动作都要问：跑命令、改文件。只读操作仍然放行。
      return [
        { permission: "bash", pattern: "*", action: "ask" },
        { permission: "apply_patch", pattern: "*", action: "ask" },
      ];
  }
}

export function parsePreset(s: string | undefined): PresetName {
  return s === "strict" || s === "auto" || s === "cautious" ? s : "cautious";
}
