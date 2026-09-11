// 子 agent 的权限收窄。
//
// ★ 单调不增（UIPI 模型，见 harness-windows-kernel-analogy.md §2.8）：
//   子 agent 的权限集合必须是父 agent 的子集。用 intersect 而不是 merge ——
//   merge 的语义是"合并两边的诉求"，只要子 agent 能往里加一条 allow，
//   一个被注入的子 agent 就能给自己提权。
//
// Windows 里低完整性进程不能驱动高完整性进程的行为；这里是同一条规矩。
import type { Rule } from "./rules.js";

export type SandboxMode = "read-only" | "workspace-write" | "danger-full-access";
export type NetMode = "deny" | "allow";

const SANDBOX_ORDER: SandboxMode[] = ["read-only", "workspace-write", "danger-full-access"];

export interface Grant {
  sandbox: SandboxMode;
  net: NetMode;
  roots: string[];
  rules: Rule[];
  tools?: Set<string>;
}

export interface GrantRequest {
  sandbox?: SandboxMode;
  net?: NetMode;
  roots?: string[];
  rules?: Rule[];
  tools?: string[];
}

function tighterSandbox(a: SandboxMode, b: SandboxMode): SandboxMode {
  return SANDBOX_ORDER.indexOf(a) <= SANDBOX_ORDER.indexOf(b) ? a : b;
}

/** 路径是否落在某个 root 之内（按路径分量边界判断）。 */
function within(path: string, root: string): boolean {
  return path === root || path.startsWith(root.endsWith("/") ? root : `${root}/`);
}

export interface IntersectResult {
  grant: Grant;
  /** 被驳回的越权诉求，如实记录下来 —— 它是子 agent 被注入的早期信号。 */
  rejected: string[];
}

export function intersect(parent: Grant, req: GrantRequest): IntersectResult {
  const rejected: string[] = [];

  // 沙箱与网络：取更严的一侧，子 agent 放宽的诉求直接忽略
  const sandbox = tighterSandbox(parent.sandbox, req.sandbox ?? parent.sandbox);
  if (req.sandbox && sandbox !== req.sandbox) {
    rejected.push(`sandbox "${req.sandbox}" is looser than parent "${parent.sandbox}"`);
  }
  const net: NetMode = parent.net === "deny" ? "deny" : (req.net ?? parent.net);
  if (req.net === "allow" && net === "deny") rejected.push('net "allow" denied: parent is "deny"');

  // 根目录：只能是父集合的子集
  let roots = parent.roots;
  if (req.roots && req.roots.length > 0) {
    const kept = req.roots.filter((r) => parent.roots.some((p) => within(r, p)));
    for (const r of req.roots) {
      if (!kept.includes(r)) rejected.push(`root "${r}" is outside the parent workspace`);
    }
    if (kept.length > 0) roots = kept;
  }

  // 规则：父规则在前保持基线；子 agent 只能追加"更严"的规则。
  // ★ 子 agent 提出的 allow 一律丢弃 —— 否则后匹配者胜会让它覆盖父级的 deny。
  const rules: Rule[] = [...parent.rules];
  for (const r of req.rules ?? []) {
    if (r.action === "allow") {
      rejected.push(`rule allow ${r.permission}:${r.pattern} dropped (subagents cannot widen)`);
      continue;
    }
    rules.push(r);
  }

  // 工具集：与父级求交
  let tools = parent.tools;
  if (req.tools) {
    const wanted = new Set(req.tools);
    for (const t of wanted) {
      if (parent.tools && !parent.tools.has(t)) rejected.push(`tool "${t}" is not available to the parent`);
    }
    tools = parent.tools ? new Set([...wanted].filter((t) => parent.tools!.has(t))) : wanted;
  }

  const grant: Grant = { sandbox, net, roots, rules, ...(tools ? { tools } : {}) };
  return { grant, rejected };
}
