// token 预算。
//
// 没有 tokenizer 也能做得准：先用字符数粗估，再用每次调用返回的真实 usage
// 反过来校准比值。中英文混合、代码、JSON 的比值差很多，写死一个常数必然失真。
import type { ModelMessage } from "../model/types.js";

const INITIAL_CHARS_PER_TOKEN = 3.5;

export class TokenEstimator {
  #ratio = INITIAL_CHARS_PER_TOKEN;
  #samples = 0;

  /** 用一次真实调用的结果校准：我们发了多少字符，服务端算了多少 token。 */
  calibrate(charsSent: number, actualTokens: number): void {
    if (actualTokens <= 0 || charsSent <= 0) return;
    const observed = charsSent / actualTokens;
    // 滑动平均，避免单次异常把比值带偏
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
      n += 16; // 每条消息的结构性开销
    }
    return n;
  }

  estimate(messages: ModelMessage[]): number {
    return Math.ceil(this.chars(messages) / this.#ratio);
  }
}

export interface CompactionSettings {
  enabled: boolean;
  /** 模型的上下文窗口总量 */
  contextWindow: number;
  /** 给回复留出的余量 */
  reserveTokens: number;
  /** 末尾这么多 token 的历史保持原文 */
  keepRecentTokens: number;
}

export const DEFAULT_COMPACTION: CompactionSettings = {
  enabled: true,
  contextWindow: 32768,
  reserveTokens: 4096,
  keepRecentTokens: 8192,
};
