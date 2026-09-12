// Context compaction.
//
// ★ Three hard rules:
//   1. Compaction may only happen during assembling (compacting while a tool
//      is mid-execution tears the state apart)
//   2. Compaction leaves a **visible, auditable** record rather than quietly
//      rewriting history -- otherwise, while debugging, you can never work out
//      what the model actually saw at the time
//   3. Never split assistant(tool_calls) from its tool results -- split them
//      and the server rejects the next request outright
import type { ModelMessage } from "../model/types.js";
import type { CompactionSettings, TokenEstimator } from "./budget.js";

export interface CompactionPlan {
  needed: boolean;
  /** History before this index is replaced by the summary (index 0, the
   *  original task, is always kept) */
  keepFrom: number;
  estimatedTokens: number;
  budget: number;
}

/**
 * Push the index to a safe boundary: it must not land on a tool message, or it
 * leaves an orphaned "result with no call" and the server errors out.
 */
export function safeBoundary(history: readonly ModelMessage[], idx: number): number {
  let i = Math.max(0, Math.min(idx, history.length));
  while (i < history.length && history[i]?.role === "tool") i++;
  return i;
}

/** Scan backwards for the last assistant message's index -- it and its tool
 *  results form one group that must not be split. */
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

  // Accumulate backwards from the end and stop once keepRecentTokens is met
  let acc = 0;
  let i = history.length;
  while (i > 1) {
    const next = est.estimate([history[i - 1]!]);
    if (acc + next > s.keepRecentTokens) break;
    acc += next;
    i--;
  }
  // ★ The backstop: the final assistant+tool group must survive.
  //   Otherwise "a large file just read is immediately compacted away" -- the
  //   model believes it never read it, reads it again, and loops forever.
  const lastGroupStart = lastAssistantIndex(history);
  const keepFrom = safeBoundary(history, Math.min(Math.max(1, i), Math.max(1, lastGroupStart)));

  // Nothing to compact means do not compact (so it does not fire repeatedly
  // with no effect)
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

/** Replace the range [1, keepFrom) with the summary. Index 0 (the original
 *  task) is always kept. */
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
