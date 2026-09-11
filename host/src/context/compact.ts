// 上下文压缩。
//
// ★ 三条硬规矩：
//   1. 压缩只能发生在 assembling 阶段（工具执行到一半压缩 = 状态撕裂）
//   2. 压缩是一条**可见、可审计**的记录，不是偷偷改历史 ——
//      否则 debug 时你永远搞不清模型当时到底看到了什么
//   3. 绝不能把 assistant(tool_calls) 和它的 tool 结果切散 ——
//      切散了下一次请求会被服务端直接拒绝
import type { ModelMessage } from "../model/types.js";
import type { CompactionSettings, TokenEstimator } from "./budget.js";

export interface CompactionPlan {
  needed: boolean;
  /** 这个下标之前的历史将被摘要替换（下标 0 的原始任务永远保留） */
  keepFrom: number;
  estimatedTokens: number;
  budget: number;
}

/**
 * 把下标推到一个安全边界上：不能停在 tool 消息上，否则会留下
 * 「有结果没有调用」的孤儿，服务端会报错。
 */
export function safeBoundary(history: readonly ModelMessage[], idx: number): number {
  let i = Math.max(0, Math.min(idx, history.length));
  while (i < history.length && history[i]?.role === "tool") i++;
  return i;
}

/** 从尾部往前找最后一条 assistant 消息的下标 —— 它和它的 tool 结果是一组，不能拆。 */
export function lastAssistantIndex(history: readonly ModelMessage[]): number {
  for (let i = history.length - 1; i >= 0; i--) {
    if (history[i]?.role === "assistant") return i;
  }
  return history.length;
}

export function planCompaction(
  history: readonly ModelMessage[],
  est: TokenEstimator,
  s: CompactionSettings,
): CompactionPlan {
  const budget = s.contextWindow - s.reserveTokens;
  const total = est.estimate([...history]);
  if (!s.enabled || total <= budget || history.length < 4) {
    return { needed: false, keepFrom: 0, estimatedTokens: total, budget };
  }

  // 从尾部往前累加，凑够 keepRecentTokens 就停
  let acc = 0;
  let i = history.length;
  while (i > 1) {
    const next = est.estimate([history[i - 1]!]);
    if (acc + next > s.keepRecentTokens) break;
    acc += next;
    i--;
  }
  // ★ 兜底：最后一组 assistant+tool 必须留下。
  //   否则"刚读完一个大文件就立刻被压掉"——模型会以为没读过，重新再读，无限循环。
  const lastGroupStart = lastAssistantIndex(history);
  const keepFrom = safeBoundary(history, Math.min(Math.max(1, i), Math.max(1, lastGroupStart)));

  // 没东西可压就别压（避免反复触发却不生效）
  if (keepFrom <= 1) return { needed: false, keepFrom: 0, estimatedTokens: total, budget };
  return { needed: true, keepFrom, estimatedTokens: total, budget };
}

export const SUMMARY_INSTRUCTION = `Summarize the conversation so far for your own future reference.

Include, concretely:
- what the task is and what has been decided
- which files were inspected or changed, and what changed in them
- commands that were run and what their results were
- what is still unfinished, and the immediate next step

Be specific — file paths, function names, error messages. Omit pleasantries.
This summary replaces the raw history: anything you leave out is lost.`;

/** 用摘要替换掉 [1, keepFrom) 这一段。下标 0（原始任务）始终保留。 */
export function applyCompaction(
  history: ModelMessage[],
  keepFrom: number,
  summary: string,
): { next: ModelMessage[]; replaced: ModelMessage[] } {
  const replaced = history.slice(1, keepFrom);
  const next: ModelMessage[] = [
    history[0]!,
    { role: "user", content: `<compacted_history>\n${summary}\n</compacted_history>` },
    ...history.slice(keepFrom),
  ];
  return { next, replaced };
}
