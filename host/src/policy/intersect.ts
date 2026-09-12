// Narrowing a subagent's permissions.
//
// ★ Monotonically non-increasing (the UIPI model; see
//   harness-windows-kernel-analogy.md section 2.8): a subagent's permission set
//   must be a subset of its parent's. Use intersect, not merge -- merge means
//   "combine what both sides asked for", and the moment a subagent can add one
//   allow, an injected subagent can escalate its own privileges.
//
// On Windows a low-integrity process cannot drive a high-integrity process's
// behaviour; this is the same rule.
import type { Rule } from "./rules.js";
import { pathWithin } from "../platform.js";

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

/**
 * Whether a path lies inside some root (judged on path-component boundaries).
 *
 * ★ This is not a string comparison, it is a **permission decision**: it
 *   determines whether a root a subagent requested counts as a subset of the
 *   parent's. On Windows the separator is ambiguous and case is insensitive,
 *   so comparing bytes lets a subagent "obtain" a new root merely by
 *   respelling the case. See platform.ts for the details.
 */
const within = pathWithin;

export interface IntersectResult {
  grant: Grant;
  /** Rejected over-reaching requests, recorded honestly -- they are an early
   *  signal that a subagent has been injected. */
  rejected: string[];
}

export function intersect(parent: Grant, req: GrantRequest): IntersectResult {
  const rejected: string[] = [];

  // Sandbox and network: take the stricter side; a subagent's request to
  // loosen is simply ignored
  const sandbox = tighterSandbox(parent.sandbox, req.sandbox ?? parent.sandbox);
  if (req.sandbox && sandbox !== req.sandbox) {
    rejected.push(`sandbox "${req.sandbox}" is looser than parent "${parent.sandbox}"`);
  }
  const net: NetMode = parent.net === "deny" ? "deny" : (req.net ?? parent.net);
  if (req.net === "allow" && net === "deny") rejected.push('net "allow" denied: parent is "deny"');

  // Roots: only a subset of the parent's set
  let roots = parent.roots;
  if (req.roots && req.roots.length > 0) {
    const kept = req.roots.filter((r) => parent.roots.some((p) => within(r, p)));
    for (const r of req.roots) {
      if (!kept.includes(r)) rejected.push(`root "${r}" is outside the parent workspace`);
    }
    if (kept.length > 0) roots = kept;
  }

  // Rules: the parent's come first and hold the baseline; a subagent may only
  // append *stricter* rules.
  // ★ Any allow a subagent proposes is discarded -- otherwise last-match-wins
  //   would let it override the parent's deny.
  const rules: Rule[] = [...parent.rules];
  for (const r of req.rules ?? []) {
    if (r.action === "allow") {
      rejected.push(`rule allow ${r.permission}:${r.pattern} dropped (subagents cannot widen)`);
      continue;
    }
    rules.push(r);
  }

  // Tool set: intersected with the parent's
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
