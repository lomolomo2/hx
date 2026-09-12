import type { Rule } from "./rules.js";

// Dangerous commands: not "forbidden" but "worth stopping to ask about".
// Selection criteria: irreversible, reaching outside the workspace, or
// publishing something externally.
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
      // Fully automatic: everything allowed. The sandbox is still there; it
      // simply stops asking about intent.
      return [];

    case "cautious":
      // The recommended default: everyday operations pass, dangerous commands
      // get a question.
      return DANGEROUS_BASH.map((pattern) => ({
        permission: "bash",
        pattern,
        action: "ask" as const,
      }));

    case "strict":
      // Every action with a side effect gets a question: running commands,
      // changing files. Read-only operations still pass.
      return [
        { permission: "bash", pattern: "*", action: "ask" },
        { permission: "apply_patch", pattern: "*", action: "ask" },
      ];
  }
}

export function parsePreset(s: string | undefined): PresetName {
  return s === "strict" || s === "auto" || s === "cautious" ? s : "cautious";
}
