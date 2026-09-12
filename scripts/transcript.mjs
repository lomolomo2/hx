// Read a rollout back as a human-readable transcript.
//
//   node scripts/transcript.mjs                 # the newest session
//   node scripts/transcript.mjs s6468           # by session name
//   node scripts/transcript.mjs path/to.jsonl   # by path
//   node scripts/transcript.mjs --list          # what sessions exist
//   node scripts/transcript.mjs s6468 --full    # do not truncate anything
//   node scripts/transcript.mjs s6468 --prompts # dump what was sent to the model
//                                               # (only if the run had HX_LOG_PROMPTS=1)
//
// ★ Why this exists. The rollout already holds every step of a run, but it is
//   JSONL built for machines: one record per line, tool calls and their results
//   separated by however many records came in between. Asking the model to
//   summarise its own run instead produces a reconstruction -- and a measured
//   one compressed 19 tool calls into "2 steps" and described results it had
//   not checked. The data was right there; only a reader was missing.
//
// ★ By default a rollout does NOT contain the text sent to the model. The
//   prompt is assembled fresh each step by buildPrompt() (system prompt +
//   world snapshot + history) and thrown away; what is recorded is the
//   conversation -- what the model said and what it got back.
//   Run with HX_LOG_PROMPTS=1 and each call is additionally written as a
//   `model_call` record, which --prompts renders here. The tool says which of
//   the two it is looking at rather than letting a reader assume.
//
// Dependency-free on purpose: it has to run from a bare clone, before any
// npm install.
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, sep } from "node:path";
import { homedir } from "node:os";

const argv = process.argv.slice(2);
const full = argv.includes("--full");
const wantList = argv.includes("--list");
const wantPrompts = argv.includes("--prompts");
const target = argv.find((a) => !a.startsWith("--"));

const SESSIONS = join(homedir(), ".hx", "sessions");

// ---------------------------------------------------------------- locating

/** Every rollout under ~/.hx/sessions, newest first. */
function allRollouts(dir = SESSIONS, acc = []) {
  let entries;
  try {
    entries = readdirSync(dir, { withFileTypes: true });
  } catch {
    return acc; // unreadable directory: skip it rather than fail the whole walk
  }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) allRollouts(p, acc);
    else if (e.name.startsWith("rollout-") && e.name.endsWith(".jsonl")) {
      acc.push({ path: p, mtime: statSync(p).mtimeMs, size: statSync(p).size });
    }
  }
  return acc.sort((a, b) => b.mtime - a.mtime);
}

function resolveTarget(t) {
  if (!t) {
    const newest = allRollouts()[0];
    if (!newest) die(`no rollouts found under ${SESSIONS}`);
    return newest.path;
  }
  // A path, if it looks like one and exists.
  if (t.includes(sep) || t.includes("/") || t.endsWith(".jsonl")) {
    try {
      statSync(t);
      return t;
    } catch {
      /* fall through to treating it as a session name */
    }
  }
  const hit = allRollouts().find((r) => r.path.endsWith(`rollout-${t}.jsonl`));
  if (!hit) die(`no rollout for session "${t}". Try --list.`);
  return hit.path;
}

function die(msg) {
  process.stderr.write(`transcript: ${msg}\n`);
  process.exit(1);
}

if (wantList) {
  const rows = allRollouts();
  if (rows.length === 0) die(`no rollouts found under ${SESSIONS}`);
  for (const r of rows) {
    const name = r.path.split(/[\\/]/).pop().replace(/^rollout-|\.jsonl$/g, "");
    const when = new Date(r.mtime).toISOString().replace("T", " ").slice(0, 19);
    process.stdout.write(`${when}  ${String(r.size).padStart(9)}  ${name}\n`);
  }
  process.exit(0);
}

// ---------------------------------------------------------------- rendering

const W = 78;
const rule = (ch = "=") => ch.repeat(W);
const out = [];
const say = (s = "") => out.push(s);

/** Truncate unless --full, and keep the cut visible rather than silent. */
function cut(s, n) {
  s = String(s ?? "");
  if (full || s.length <= n) return s;
  return `${s.slice(0, n)}\n… [${s.length - n} more chars; re-run with --full]`;
}

const indent = (s, pad) =>
  String(s ?? "")
    .split("\n")
    .map((l) => pad + l)
    .join("\n");

/** A one-line description of a call's arguments, per tool. */
function describeArgs(name, args) {
  if (name === "bash") return String(args.cmd ?? "");
  if (name === "apply_patch") {
    const patch = String(args.patch ?? "");
    const files = [...patch.matchAll(/^\*\*\* (Add|Update|Delete) File: (.+)$/gm)]
      .map((m) => `${m[1].toLowerCase()} ${m[2]}`);
    const plus = (patch.match(/^\+/gm) || []).length;
    const minus = (patch.match(/^-(?!--)/gm) || []).length;
    return `${files.join("; ")}  (+${plus} -${minus})`;
  }
  if (name === "read") {
    const bits = [args.path];
    if (args.offset) bits.push(`offset=${args.offset}`);
    if (args.limit) bits.push(`limit=${args.limit}`);
    return bits.join("  ");
  }
  if (name === "glob" || name === "grep") return args.pattern ?? JSON.stringify(args);
  if (name === "todo") return `${(args.items ?? []).length} item(s)`;
  if (name === "task") return `${args.name ?? "?"}: ${args.instructions ?? ""}`;
  return JSON.stringify(args);
}

const file = resolveTarget(target);
const lines = readFileSync(file, "utf8").split(/\r?\n/).filter((l) => l.trim());

// Pair each call with its result up front: in the record stream a model turn
// may emit several calls before any result arrives, so rendering them in
// arrival order puts outputs under the wrong call.
const results = new Map();
for (const line of lines) {
  let r;
  try {
    r = JSON.parse(line);
  } catch {
    continue;
  }
  if (r.type === "response_item" && r.payload?.type === "function_call_output") {
    results.set(r.payload.call_id, r.payload.output);
  }
}

const stats = { calls: 0, byTool: new Map(), compactions: 0, turns: 0, modelCalls: 0 };
let step = 0;

say(`file  ${file}`);

for (const line of lines) {
  let r;
  try {
    r = JSON.parse(line);
  } catch {
    say(`  [unparseable line skipped]`);
    continue;
  }
  const p = r.payload ?? {};

  switch (r.type) {
    case "session_meta": {
      const e = p.effective ?? {};
      const when = r.ts ? new Date(r.ts).toISOString().replace("T", " ").slice(0, 19) : "";
      say(`\nsession ${p.session ?? "?"}${when ? `   ${when}` : ""}`);
      say(`  roots     ${(e.roots ?? []).join(", ")}`);
      say(`  sandbox   ${e.sandbox} / net=${e.net}   enforced=${e.enforced}   backend=${e.backend}`);
      if (e.extra_read_paths?.length) say(`  read also ${e.extra_read_paths.join(", ")}`);
      // Warnings are the engine telling you what it could NOT do. They are the
      // first thing to read when a run behaved strangely.
      for (const w of p.warnings ?? []) say(`  ! ${w}`);
      break;
    }

    case "event":
      switch (p.type) {
        case "user_message":
          stats.turns++;
          say(`\n${rule()}\nUSER  ${cut(p.text, 2000)}\n${rule()}`);
          break;
        case "agent_message":
          say(`\n${rule("-")}\nANSWER\n${indent(cut(p.text, 2500), "  ")}`);
          break;
        case "approval_requested":
          say(`\n      ⚠ approval requested: ${p.tool}  [${(p.subjects ?? []).join(", ")}]`);
          break;
        case "approval_resolved":
          say(`      ⚠ approval ${p.decision}`);
          break;
        case "policy_denied":
          say(`\n      ⛔ policy denied: ${p.tool}  [${(p.subjects ?? []).join(", ")}]`);
          break;
        case "subagent_spawned": {
          const g = p.grant ?? {};
          say(`\n      ↳ subagent "${p.name}"  ${g.sandbox}/net=${g.net}`);
          say(`        roots ${(g.roots ?? []).join(", ")}`);
          say(`        tools ${(g.tools ?? []).join(" ")}`);
          // The rejected list is the whole point of intersect(): a subagent
          // asking for more than its parent had is an early signal that it was
          // injected, so it is surfaced rather than buried.
          for (const x of p.rejected ?? []) say(`        ⛔ refused: ${x}`);
          break;
        }
        case "turn_aborted":
          say(`\n      ⏹ turn aborted by user`);
          break;
        case "turn_failed":
          say(`\n      ✗ turn failed: ${p.message}`);
          break;
        default:
          say(`\n      [${p.type}] ${cut(JSON.stringify(p), 300)}`);
      }
      break;

    case "model_call": {
      // Only present when the run had HX_LOG_PROMPTS=1. This is the one record
      // that holds what the model was actually sent.
      stats.modelCalls++;
      const req = p.request ?? {};
      const res = p.response ?? {};
      const n = (req.messages ?? []).length;
      const stepLabel = p.step ? `step ${p.step.current}/${p.step.max}` : p.kind;
      if (p.dropped) {
        say(`\n      ↑ model call (${stepLabel}) -- ${p.dropped}`);
      } else if (!wantPrompts) {
        say(
          `\n      ↑ model call (${stepLabel}): ${n} message(s), ${(req.tools ?? []).length} tool(s)` +
            ` → in=${res.usage?.inputTokens} out=${res.usage?.outputTokens}  ${p.ms}ms` +
            `   [--prompts to see it]`,
        );
      } else {
        say(`\n${rule("-")}`);
        say(`MODEL CALL  ${stepLabel}  (${p.kind})  ${p.model}`);
        say(`${rule("-")}`);
        for (const m of req.messages ?? []) {
          say(`\n  [${m.role}]`);
          if (m.content) say(indent(cut(m.content, 4000), "    "));
          for (const tc of m.toolCalls ?? []) {
            say(indent(`(tool_call ${tc.name}) ${cut(tc.argumentsJson, 1500)}`, "    "));
          }
        }
        say(`\n  [tools offered] ${(req.tools ?? []).map((t) => t?.function?.name ?? t?.name ?? "?").join(" ")}`);
        say(`\n  [response] in=${res.usage?.inputTokens} out=${res.usage?.outputTokens} ${p.ms}ms`);
        if (res.reasoning) say(indent(`reasoning: ${cut(res.reasoning, 2000)}`, "    "));
        if (res.content) say(indent(cut(res.content, 4000), "    "));
        for (const tc of res.tool_calls ?? []) {
          say(indent(`(tool_call ${tc.name}) ${cut(tc.argumentsJson, 1500)}`, "    "));
        }
        say(`${rule("-")}`);
      }
      break;
    }

    case "compacted":
      stats.compactions++;
      say(
        `\n  ⧗ context compacted: ${p.replaced_count} messages replaced, ` +
          `~${p.tokens_before} → ~${p.tokens_after} tokens`,
      );
      if (full && p.summary) say(indent(`summary: ${p.summary}`, "      "));
      break;

    case "response_item": {
      if (p.type === "function_call_output") break; // rendered with its call
      if (p.role !== "assistant") break;

      const prose = String(p.content ?? "").trim();
      if (prose) say(`\n  · ${indent(cut(prose, 700), "    ").trimStart()}`);
      for (const tc of p.tool_calls ?? []) {
        step++;
        stats.calls++;
        stats.byTool.set(tc.name, (stats.byTool.get(tc.name) ?? 0) + 1);
        let args;
        try {
          args = JSON.parse(tc.argumentsJson);
        } catch {
          args = { raw: tc.argumentsJson };
        }
        say(`\n  ${String(step).padStart(3)}  ${tc.name.padEnd(12)} ${cut(describeArgs(tc.name, args), 400)}`);

        const res = results.get(tc.id);
        if (res === undefined) {
          // No result recorded: the run was killed, or the turn hit max steps
          // between the call and its answer.
          say(`       (no result recorded -- run ended before it came back)`);
        } else {
          say(indent(cut(res, 1200), "       | "));
        }
      }
      break;
    }

    default:
      say(`  [unknown record type ${r.type}]`);
  }
}

say(`\n${rule()}`);
const tools = [...stats.byTool.entries()].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}×${v}`);
say(`${stats.turns} user turn(s), ${stats.calls} tool call(s): ${tools.join("  ")}`);
if (stats.compactions) say(`${stats.compactions} context compaction(s)`);
if (stats.modelCalls) say(`${stats.modelCalls} model call(s) recorded${wantPrompts ? "" : " -- add --prompts to see them"}`);
say(
  stats.modelCalls
    ? `\nNote: model_call records hold the assembled messages and tool schemas --\n` +
        `the semantic input. They are not a byte-level capture of the HTTP body;\n` +
        `the transport adds the model name and sampling parameters of its own.`
    : `\nNote: this run did not record prompts, so the above is the conversation,\n` +
        `not the exact input to the model. Each step's prompt is assembled fresh by\n` +
        `buildPrompt() and is not written to disk unless HX_LOG_PROMPTS=1.`,
);

process.stdout.write(out.join("\n") + "\n");
