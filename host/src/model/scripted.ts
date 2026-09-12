// A scriptable fake model, making end-to-end tests deterministic, offline and
// free.
//
// A real model verifies "is it good"; a fake model verifies "is it correct" --
// and the latter is what a regression test is for.
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
