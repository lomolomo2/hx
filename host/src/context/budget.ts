// The token budget.
//
// This can be accurate without a tokenizer: estimate roughly from character
// counts, then calibrate the ratio backwards from the real usage each call
// returns. The ratio differs a great deal between mixed-script prose, code and
// JSON, so a hardcoded constant is bound to be wrong.
import type { ModelMessage } from "../model/types.js";

const INITIAL_CHARS_PER_TOKEN = 3.5;

export class TokenEstimator {
  #ratio = INITIAL_CHARS_PER_TOKEN;
  #samples = 0;

  /** Calibrate from one real call: how many characters we sent, how many
   *  tokens the server counted. */
  calibrate(charsSent: number, actualTokens: number): void {
    if (actualTokens <= 0 || charsSent <= 0) return;
    const observed = charsSent / actualTokens;
    // A running average, so one outlier does not drag the ratio off
    this.#samples++;
    const weight = Math.min(0.5, 1 / this.#samples);
    this.#ratio = this.#ratio * (1 - weight) + observed * weight;
  }

  get ratio(): number {
    return this.#ratio;
  }

  chars(messages: ModelMessage[]): number {
    let n = 0;
    for (const m of messages) {
      n += (m.content ?? "").length;
      for (const t of m.toolCalls ?? []) n += t.name.length + t.argumentsJson.length;
      n += 16; // structural overhead per message
    }
    return n;
  }

  estimate(messages: ModelMessage[]): number {
    return Math.ceil(this.chars(messages) / this.#ratio);
  }
}

export interface CompactionSettings {
  enabled: boolean;
  /** The model's total context window */
  contextWindow: number;
  /** Headroom reserved for the reply */
  reserveTokens: number;
  /** This many tokens of trailing history are kept verbatim */
  keepRecentTokens: number;
}

export const DEFAULT_COMPACTION: CompactionSettings = {
  enabled: true,
  contextWindow: 32768,
  reserveTokens: 4096,
  keepRecentTokens: 8192,
};
