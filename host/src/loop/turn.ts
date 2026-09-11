// Turn 循环。
//
// ★ 阶段是显式状态，不是隐含约定：
//     assembling → streaming → executing → settling
//   "在工具执行到一半时压缩上下文"是真实存在的 bug 类别，和"在 DISPATCH_LEVEL
//   访问分页内存"是同一种错误。把阶段建出来，这类错误在写的时候就没了。
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
  /** 策略规则。后匹配者胜，所以用户规则应排在默认规则之后。 */
  rules?: Rule[];
  /** ask 时如何征求同意。不提供则 ask 等价于 deny。 */
  approval?: ApprovalHandler;
  /** hxd 路径。子 agent 会用它另起一个引擎进程（上下文隔离）。 */
  enginePath?: string;
  /** 当前嵌套深度，由父 agent 传入 */
  depth?: number;
  /** 最大嵌套深度。到顶后 task 工具不再可用。 */
  maxDepth?: number;
  /** 一轮里最多派多少个子 agent */
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

  /** 会话内的临时授权也是一条规则，追加在最后所以优先级最高。 */
  get rules(): readonly Rule[] {
    return this.#rules;
  }

  /**
   * 请求中断。
   *
   * 在阶段边界生效，而不是当场掐断 —— 工具执行到一半强行中止会留下
   * 半完成的副作用，而引擎那边并不知情。已经在跑的命令交给引擎的超时与
   * exec.kill 收尾。
   */
  abort(): void {
    this.#aborted = true;
    for (const cell of this.#runningCells) void this.engine.execKill(cell, "TERM").catch(() => {});
  }

  get aborted(): boolean {
    return this.#aborted;
  }

  /** 本轮真正会暴露给模型的工具名。策略隐藏与深度上限都体现在这里。 */
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

  /** 策略里被无差别禁掉的工具，直接不出现在工具清单里（access mask 模型）。 */
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
   * 执行前的策略闸门。返回 null 表示放行，返回字符串表示拦下并把这段话喂回模型。
   * ★ 只能在 executing 阶段调用。
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

    // 没有 handler 时 ask 等价于 deny：没人能回答却放行，等于规则不存在
    const decision = this.opts.approval ? await this.opts.approval(req) : "deny";

    this.#emit({ type: "approval.resolved", id: req.id, decision });
    this.#log("event", { type: "approval_resolved", id: req.id, decision });

    if (decision === "deny") return denialMessage(toolName, preview);
    if (decision === "allow_always") {
      // 追加在规则末尾 —— 后匹配者胜，于是同样的调用之后不再打扰用户
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

  /** 两条流分离：给模型的进 response_item，给人的进 event。 */
  #log(type: "response_item" | "event" | "compacted", payload: unknown): void {
    void this.engine.logAppend({ type, payload }).catch(() => {
      /* 日志不该拖垮主流程；引擎侧失败已在 stderr 报告 */
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
      landlockAbi: Number(eff["landlock_abi"]),
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
      // 让模型看见自己的预算 —— 看不见预算就不会收敛（实测：三次跑完 40 步、0 产出）
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
      // 用真实 usage 校准估算比值 —— 下一轮的压缩判断就更准
      this.#est.calibrate(this.#est.chars(messages), assistant.usage.inputTokens);

      if (assistant.reasoning) {
        this.#emit({
          type: "item.completed",
          item: { id: `i${++this.#itemSeq}`, type: "reasoning", text: assistant.reasoning },
        });
      }

      // 助手消息必须原样回灌（含 tool_calls），否则模型会重复调同一个工具
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

        // 推理模型可能把 token 全花在思考上，content 是空的。
        // 那不是"回答完了"，是"被截断了" —— 提醒一次再继续，别把空串当成答案。
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
          // 理论上不该发生：被禁的工具根本不在清单里
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
            // ★ 工具失败也要喂回去，绝不抛穿循环
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
   * 派一个子 agent。
   *
   * ★ 三条硬规矩：
   *   1. 权限取交集（policy/intersect.ts）—— 子 agent 不可能比父 agent 权限大
   *   2. 另起一个引擎进程 + 独立 rollout —— 上下文不共享，只回传结论
   *   3. 回传内容包上 <subagent_result trust="data">，声明它是数据不是指令
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

    // ★ 信任降级：子 agent 可能读到被注入的内容，它的回复不能当指令用
    const note = rejected.length > 0 ? `\n(note: the subagent asked for privileges it did not get: ${rejected.join("; ")})` : "";
    return `<subagent_result name="${req.name}" trust="data">\n${report}\n</subagent_result>${note}`;
  }

  /** ★ 只能在 assembling 阶段调用：工具执行到一半改历史会撕裂状态。 */
  async #maybeCompact(): Promise<void> {
    this.#requirePhase("assembling", "compaction");
    const plan = planCompaction(this.#history, this.#est, this.#compaction);
    if (!plan.needed) return;

    const toSummarize = this.#history.slice(1, plan.keepFrom);
    let summary: string;
    try {
      // ★ 必须把原始任务一并交给摘要模型。
      //   漏掉它时，模型在摘要里写下了 "The original task statement isn't in the
      //   visible history"，然后丢掉目标、重新探索、陷入死循环。
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
      // 压缩失败不能让整轮失败：退化成"丢掉中段并如实说明"
      summary = `(summarization failed: ${(e as Error).message}; ${toSummarize.length} earlier messages were dropped)`;
    }
    if (summary === "") {
      summary = `(summarizer returned nothing; ${toSummarize.length} earlier messages were dropped)`;
    }

    const { next, replaced } = applyCompaction(this.#history, plan.keepFrom, summary);
    const after = this.#est.estimate(next);

    // ★ 压缩必须留下可审计的记录，而不是偷偷改历史
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
