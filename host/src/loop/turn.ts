// The turn loop.
//
// ★ Phases are explicit state, not an implicit convention:
//     assembling -> streaming -> executing -> settling
//   "Compacting the context while a tool is mid-execution" is a real class of
//   bug, and the same kind of error as "touching paged memory at
//   DISPATCH_LEVEL". Build the phases out and that class of error disappears
//   at the point of writing.
import type { ModelClient, ModelMessage } from "../model/types.js";
import type { SandboxReport, ThreadEvent, Usage } from "../protocol/events.js";
import type { ToolRegistry } from "../tools/registry.js";
import type { ToolContext } from "../tools/types.js";
import { buildPrompt, type WorldState } from "../prompt/build.js";
import { DEFAULT_COMPACTION, TokenEstimator, type CompactionSettings } from "../context/budget.js";
import { denialMessage, type ApprovalHandler, type ApprovalRequest } from "../policy/approval.js";
import { evaluateAll, visibleToolNames, type Rule } from "../policy/rules.js";
import { intersect, type Grant, type NetMode, type SandboxMode } from "../policy/intersect.js";
import { EngineClient } from "../engine/client.js";
import type { SubagentRequest } from "../tools/types.js";
import { SUMMARY_INSTRUCTION, applyCompaction, planCompaction } from "../context/compact.js";

export type TurnPhase = "assembling" | "streaming" | "executing" | "settling";

export class PhaseError extends Error {}

export interface AgentOptions {
  maxSteps?: number;
  allowedTools?: Set<string>;
  onEvent?: (e: ThreadEvent) => void;
  compaction?: Partial<CompactionSettings>;
  /** Policy rules. Last match wins, so user rules belong after the defaults. */
  rules?: Rule[];
  /** How consent is sought on ask. Without one, ask is equivalent to deny. */
  approval?: ApprovalHandler;
  /** Path to hxd. A subagent uses it to start a separate engine process
   *  (context isolation). */
  enginePath?: string;
  /** The current nesting depth, passed in by the parent agent */
  depth?: number;
  /** Maximum nesting depth. At the cap the task tool is no longer available. */
  maxDepth?: number;
  /** How many subagents may be dispatched in one turn */
  maxSubagents?: number;
}

export interface RunResult {
  finalMessage: string | null;
  steps: number;
  usage: Usage;
  stoppedBecause: "final_message" | "max_steps" | "aborted";
}

export class Agent {
  #phase: TurnPhase = "settling";
  #itemSeq = 0;
  #turnSeq = 0;
  #history: ModelMessage[] = [];
  #todos: { text: string; completed: boolean }[] = [];
  #world: WorldState | null = null;
  #sandbox: SandboxReport | null = null;
  #est = new TokenEstimator();
  #compaction: CompactionSettings;
  #rules: Rule[];
  #approvalSeq = 0;
  #openOpts: { roots: string[]; sandbox: string; net: string } | null = null;
  #subagentCount = 0;
  #aborted = false;
  #runningCells = new Set<string>();

  constructor(
    private readonly engine: EngineClient,
    private readonly model: ModelClient,
    private readonly tools: ToolRegistry,
    private readonly opts: AgentOptions = {},
  ) {
    this.#compaction = { ...DEFAULT_COMPACTION, ...opts.compaction };
    this.#rules = [...(opts.rules ?? [])];
  }

  /** A temporary in-session grant is a rule too, appended last so it has the
   *  highest precedence. */
  get rules(): readonly Rule[] {
    return this.#rules;
  }

  /**
   * Request an interrupt.
   *
   * It takes effect at a phase boundary rather than cutting in immediately --
   * forcibly aborting a tool mid-execution leaves half-finished side effects
   * the engine knows nothing about. Commands already running are wound up by
   * the engine's timeout and exec.kill.
   */
  abort(): void {
    this.#aborted = true;
    for (const cell of this.#runningCells) void this.engine.execKill(cell, "TERM").catch(() => {});
  }

  get aborted(): boolean {
    return this.#aborted;
  }

  /** The tool names actually exposed to the model this turn. Both policy
   *  hiding and the depth cap show up here. */
  get availableTools(): string[] {
    return [...this.#visibleTools()].sort();
  }

  get history(): readonly ModelMessage[] {
    return this.#history;
  }
  get phase(): TurnPhase {
    return this.#phase;
  }
  get sandbox(): SandboxReport | null {
    return this.#sandbox;
  }

  /** Tools the policy denies outright simply do not appear in the tool list
   *  (the access-mask model). */
  #canSpawn(): boolean {
    const depth = this.opts.depth ?? 0;
    const maxDepth = this.opts.maxDepth ?? 2;
    return Boolean(this.opts.enginePath) && depth < maxDepth;
  }

  #visibleTools(): Set<string> {
    const all = this.tools.visible().map((t) => t.name).filter((n) => n !== "task" || this.#canSpawn());
    const byPolicy = visibleToolNames(all, this.#rules);
    if (!this.opts.allowedTools) return byPolicy;
    return new Set([...byPolicy].filter((n) => this.opts.allowedTools!.has(n)));
  }

  /**
   * The policy gate, before execution. Returning null allows it; returning a
   * string blocks it and feeds that text back to the model.
   * ★ May only be called during the executing phase.
   */
  async #gate(toolName: string, args: Record<string, unknown>): Promise<string | null> {
    this.#requirePhase("executing", "policy gate");
    const tool = this.tools.get(toolName);
    const subjects = tool?.subjects?.(args) ?? [];
    const action = evaluateAll(toolName, subjects, this.#rules);
    if (action === "allow") return null;

    const preview = tool?.preview?.(args) ?? `${toolName} ${subjects.join(" ")}`;

    if (action === "deny") {
      this.#log("event", { type: "policy_denied", tool: toolName, subjects });
      return denialMessage(toolName, preview);
    }

    // ---- ask ----
    const req: ApprovalRequest = {
      id: `a${++this.#approvalSeq}`,
      tool: toolName,
      subjects,
      preview,
    };
    this.#emit({ type: "approval.requested", id: req.id, tool: toolName, preview });
    this.#log("event", { type: "approval_requested", ...req });

    // With no handler, ask is equivalent to deny: allowing something nobody
    // can answer is the same as the rule not existing
    const decision = this.opts.approval ? await this.opts.approval(req) : "deny";

    this.#emit({ type: "approval.resolved", id: req.id, decision });
    this.#log("event", { type: "approval_resolved", id: req.id, decision });

    if (decision === "deny") return denialMessage(toolName, preview);
    if (decision === "allow_always") {
      // Appended at the end of the rules -- last match wins, so the same call
      // stops bothering the user from now on
      for (const s of subjects.length > 0 ? subjects : ["*"]) {
        this.#rules.push({ permission: toolName, pattern: s, action: "allow" });
      }
    }
    return null;
  }

  #emit(e: ThreadEvent): void {
    this.opts.onEvent?.(e);
  }

  #requirePhase(want: TurnPhase, what: string): void {
    if (this.#phase !== want) {
      throw new PhaseError(`${what} is only allowed in phase "${want}" (now "${this.#phase}")`);
    }
  }

  /** The two streams stay separate: what the model sees goes to response_item,
   *  what people see goes to event. */
  #log(type: "response_item" | "event" | "compacted", payload: unknown): void {
    void this.engine.logAppend({ type, payload }).catch(() => {
      /* logging must not drag down the main path; engine-side failures are
         already reported on stderr */
    });
  }

  async open(opts: {
    roots: string[];
    sandbox: string;
    net: string;
    name?: string;
    extraReadPaths?: string[];
  }): Promise<void> {
    const r = (await this.engine.sessionOpen(opts)) as {
      session: string;
      rollout: string;
      effective: Record<string, unknown>;
      warnings?: string[];
    };
    const eff = r.effective;
    this.#sandbox = {
      sandbox: String(eff["sandbox"]),
      net: String(eff["net"]),
      enforced: Boolean(eff["enforced"]),
      netEnforced: Boolean(eff["net_enforced"]),
      backend: String(eff["backend"] ?? "none"),
      warnings: r.warnings ?? [],
    };
    this.#world = {
      cwd: opts.roots[0] ?? ".",
      roots: opts.roots,
      sandbox: this.#sandbox,
      date: new Date().toISOString().slice(0, 10),
      toolNames: this.tools.visible(this.#visibleTools()).map((t) => t.name),
      todos: this.#todos,
    };
    this.#openOpts = { roots: opts.roots, sandbox: opts.sandbox, net: opts.net };
    this.#emit({ type: "thread.started", threadId: r.session, rollout: r.rollout, sandbox: this.#sandbox });
  }

  async run(userText: string): Promise<RunResult> {
    if (!this.#world) throw new Error("call open() first");
    const maxSteps = this.opts.maxSteps ?? 20;
    const usage: Usage = { inputTokens: 0, outputTokens: 0 };
    const turnId = `t${++this.#turnSeq}`;

    this.#emit({ type: "turn.started", turnId });
    this.#history.push({ role: "user", content: userText });
    this.#log("event", { type: "user_message", text: userText });

    const ctx: ToolContext = {
      engine: this.engine,
      emit: (e) => this.#emit(e),
      nextItemId: () => `i${++this.#itemSeq}`,
      todos: this.#todos,
      ...(this.#canSpawn() ? { spawnSubagent: (req) => this.#spawnSubagent(req) } : {}),
    };

    this.#aborted = false;
    for (let step = 0; step < maxSteps; step++) {
      if (this.#aborted) {
        this.#phase = "settling";
        this.#emit({ type: "turn.failed", error: { message: "aborted by user" } });
        this.#log("event", { type: "turn_aborted", step });
        return { finalMessage: null, steps: step, usage, stoppedBecause: "aborted" };
      }
      // ---------- assembling ----------
      this.#phase = "assembling";
      // Let the model see its own budget -- without it, it does not converge
      // (measured: three runs went the full 40 steps and produced nothing)
      this.#world.step = { current: step + 1, max: maxSteps };
      await this.#maybeCompact();
      const messages = buildPrompt({ world: this.#world, history: this.#history });
      const schemas = this.tools.schemas(this.#visibleTools());

      // ---------- streaming ----------
      this.#phase = "streaming";
      let assistant;
      try {
        assistant = await this.model.complete(messages, schemas);
      } catch (e) {
        const message = (e as Error).message;
        this.#emit({ type: "turn.failed", error: { message } });
        this.#log("event", { type: "turn_failed", message });
        throw e;
      }
      usage.inputTokens += assistant.usage.inputTokens;
      usage.outputTokens += assistant.usage.outputTokens;
      // Calibrate the estimation ratio against real usage -- the next turn's
      // compaction decision is then more accurate
      this.#est.calibrate(this.#est.chars(messages), assistant.usage.inputTokens);

      if (assistant.reasoning) {
        this.#emit({
          type: "item.completed",
          item: { id: `i${++this.#itemSeq}`, type: "reasoning", text: assistant.reasoning },
        });
      }

      // The assistant message must be fed back verbatim (tool_calls included),
      // or the model calls the same tool again
      this.#history.push({
        role: "assistant",
        content: assistant.content,
        ...(assistant.toolCalls.length > 0 ? { toolCalls: assistant.toolCalls } : {}),
      });
      this.#log("response_item", {
        type: "message",
        role: "assistant",
        content: assistant.content,
        tool_calls: assistant.toolCalls,
      });

      if (assistant.toolCalls.length === 0) {
        // ---------- settling ----------
        this.#phase = "settling";
        const text = assistant.content ?? "";

        // A reasoning model may spend all its tokens thinking and leave
        // content empty. That is not "the answer is finished", it is "this was
        // truncated" -- nudge once and continue rather than treating an empty
        // string as the answer.
        if (text.trim() === "") {
          this.#history.push({
            role: "user",
            content:
              assistant.finishReason === "length"
                ? "Your reply was cut off before any answer. Be brief: either call a tool or give the final answer."
                : "You returned an empty reply. Either call a tool or give the final answer.",
          });
          continue;
        }

        this.#emit({
          type: "item.completed",
          item: { id: `i${++this.#itemSeq}`, type: "agent_message", text },
        });
        this.#emit({ type: "turn.completed", usage });
        this.#log("event", { type: "agent_message", text });
        return { finalMessage: text, steps: step + 1, usage, stoppedBecause: "final_message" };
      }

      // ---------- executing ----------
      this.#phase = "executing";
      for (const call of assistant.toolCalls) {
        if (this.#aborted) {
          this.#pushToolResult(call.id, "aborted by user before execution");
          continue;
        }
        const tool = this.tools.get(call.name);
        let content: string;

        if (!tool) {
          content = `unknown tool: ${call.name}`;
        } else if (this.opts.allowedTools && !this.opts.allowedTools.has(call.name)) {
          // Should not happen in theory: a denied tool is not in the list at all
          content = `tool ${call.name} is not permitted in this session`;
        } else {
          let args: Record<string, unknown> = {};
          try {
            args = JSON.parse(call.argumentsJson || "{}") as Record<string, unknown>;
          } catch {
            content = `invalid JSON arguments for ${call.name}`;
            this.#pushToolResult(call.id, content);
            continue;
          }
          const blocked = await this.#gate(call.name, args);
          if (blocked !== null) {
            this.#pushToolResult(call.id, blocked);
            continue;
          }
          try {
            const result = await tool.execute(args, ctx);
            content = result.content;
          } catch (e) {
            // ★ A tool failure is fed back too, and never thrown through the loop
            content = `tool ${call.name} failed: ${(e as Error).message}`;
          }
        }
        this.#pushToolResult(call.id, content);
      }

      this.#phase = "settling";
    }

    this.#emit({ type: "turn.completed", usage });
    return { finalMessage: null, steps: maxSteps, usage, stoppedBecause: "max_steps" };
  }

  /**
   * Dispatch a subagent.
   *
   * ★ Three hard rules:
   *   1. Permissions are intersected (policy/intersect.ts) -- a subagent can
   *      never have more privilege than its parent
   *   2. A separate engine process and its own rollout -- context is not
   *      shared, only conclusions come back
   *   3. What comes back is wrapped in <subagent_result trust="data">,
   *      declaring it as data rather than instructions
   */
  async #spawnSubagent(req: SubagentRequest): Promise<string> {
    const maxSubagents = this.opts.maxSubagents ?? 4;
    if (this.#subagentCount >= maxSubagents) {
      return `subagent limit reached (${maxSubagents} per session)`;
    }
    if (!this.#openOpts || !this.opts.enginePath) return "subagents are not available here";
    this.#subagentCount++;

    const parent: Grant = {
      sandbox: this.#openOpts.sandbox as SandboxMode,
      net: this.#openOpts.net as NetMode,
      roots: this.#openOpts.roots,
      rules: this.#rules,
      tools: this.#visibleTools(),
    };
    const { grant, rejected } = intersect(parent, {
      ...(req.sandbox ? { sandbox: req.sandbox } : {}),
      ...(req.tools ? { tools: req.tools } : {}),
    });

    const itemId = `i${++this.#itemSeq}`;
    this.#emit({
      type: "item.started",
      item: { id: itemId, type: "subagent", name: req.name, instructions: req.instructions, status: "in_progress" },
    });
    this.#log("event", {
      type: "subagent_spawned",
      name: req.name,
      grant: { sandbox: grant.sandbox, net: grant.net, roots: grant.roots, tools: [...(grant.tools ?? [])] },
      rejected,
    });

    const engine = new EngineClient(this.opts.enginePath);
    let report: string;
    let ok = true;
    try {
      const child = new Agent(engine, this.model, this.tools, {
        maxSteps: this.opts.maxSteps ?? 20,
        rules: grant.rules,
        ...(grant.tools ? { allowedTools: grant.tools } : {}),
        ...(this.opts.approval ? { approval: this.opts.approval } : {}),
        ...(this.opts.compaction ? { compaction: this.opts.compaction } : {}),
        enginePath: this.opts.enginePath,
        depth: (this.opts.depth ?? 0) + 1,
        maxDepth: this.opts.maxDepth ?? 2,
      });
      await child.open({
        roots: grant.roots,
        sandbox: grant.sandbox,
        net: grant.net,
        name: `sub-${req.name.replace(/[^A-Za-z0-9_-]/g, "-").slice(0, 40)}-${this.#subagentCount}`,
      });
      const r = await child.run(req.instructions);
      report = r.finalMessage ?? "(subagent produced no final report)";
      ok = r.stoppedBecause === "final_message";
    } catch (e) {
      report = `subagent error: ${(e as Error).message}`;
      ok = false;
    } finally {
      engine.close();
    }

    this.#emit({
      type: "item.completed",
      item: {
        id: itemId,
        type: "subagent",
        name: req.name,
        instructions: req.instructions,
        status: ok ? "completed" : "failed",
        result: report,
        ...(rejected.length > 0 ? { rejected } : {}),
      },
    });

    // ★ Trust downgrade: a subagent may have read injected content, so its
    //   reply must never be treated as instructions
    const note = rejected.length > 0 ? `\n(note: the subagent asked for privileges it did not get: ${rejected.join("; ")})` : "";
    return `<subagent_result name="${req.name}" trust="data">\n${report}\n</subagent_result>${note}`;
  }

  /** ★ May only be called during assembling: rewriting history while a tool is
   *  mid-execution tears the state apart. */
  async #maybeCompact(): Promise<void> {
    this.#requirePhase("assembling", "compaction");
    const plan = planCompaction(this.#history, this.#est, this.#compaction);
    if (!plan.needed) return;

    const toSummarize = this.#history.slice(1, plan.keepFrom);
    let summary: string;
    try {
      // ★ The original task must be handed to the summarizing model as well.
      //   With it missing, the model wrote "The original task statement isn't
      //   in the visible history" into its summary, then dropped the goal,
      //   started exploring again, and looped forever.
      const r = await this.model.complete(
        [
          this.#history[0] ?? { role: "user", content: "(task unavailable)" },
          ...toSummarize,
          { role: "user", content: SUMMARY_INSTRUCTION },
        ],
        [],
      );
      summary = (r.content ?? "").trim();
    } catch (e) {
      // A failed compaction must not fail the whole turn: degrade to "drop the
      // middle section and say so honestly"
      summary = `(summarization failed: ${(e as Error).message}; ${toSummarize.length} earlier messages were dropped)`;
    }
    if (summary === "") {
      summary = `(summarizer returned nothing; ${toSummarize.length} earlier messages were dropped)`;
    }

    const { next, replaced } = applyCompaction(this.#history, plan.keepFrom, summary);
    const after = this.#est.estimate(next);

    // ★ Compaction must leave an auditable record rather than quietly
    //   rewriting history
    this.#log("compacted", {
      replaced_count: replaced.length,
      tokens_before: plan.estimatedTokens,
      tokens_after: after,
      summary,
      replacement_history: replaced,
    });
    this.#emit({
      type: "context.compacted",
      replacedMessages: replaced.length,
      tokensBefore: plan.estimatedTokens,
      tokensAfter: after,
    });

    this.#history = next;
  }

  #pushToolResult(toolCallId: string, content: string): void {
    this.#history.push({ role: "tool", content, toolCallId });
    this.#log("response_item", { type: "function_call_output", call_id: toolCallId, output: content });
  }
}
