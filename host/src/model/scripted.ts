// 可脚本化的假模型：让端到端测试确定、离线、免费。
//
// 真实模型用来验"好不好"，假模型用来验"对不对" —— 后者才是回归测试该做的事。
import type { AssistantTurn, ModelClient, ModelMessage } from "./types.js";

export type Script = (messages: ModelMessage[], step: number) => AssistantTurn;

export class ScriptedModelClient implements ModelClient {
  readonly name = "scripted";
  #step = 0;
  readonly seen: ModelMessage[][] = [];

  constructor(private readonly script: Script) {}

  async complete(messages: ModelMessage[]): Promise<AssistantTurn> {
    this.seen.push(messages);
    return this.script(messages, this.#step++);
  }
}

export function say(text: string): AssistantTurn {
  return { content: text, toolCalls: [], usage: { inputTokens: 0, outputTokens: 0 } };
}

export function callTool(id: string, name: string, args: Record<string, unknown>): AssistantTurn {
  return {
    content: null,
    toolCalls: [{ id, name, argumentsJson: JSON.stringify(args) }],
    usage: { inputTokens: 0, outputTokens: 0 },
  };
}
