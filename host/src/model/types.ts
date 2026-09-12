export interface ToolCall {
  id: string;
  name: string;
  argumentsJson: string;
}

export interface ModelMessage {
  role: "system" | "user" | "assistant" | "tool";
  content: string | null;
  toolCalls?: ToolCall[];
  toolCallId?: string;
}

export interface AssistantTurn {
  content: string | null;
  /** Reasoning models (Qwen3 / DeepSeek-R1 and the like) put their thinking
   *  here, separately. */
  reasoning?: string;
  toolCalls: ToolCall[];
  finishReason?: string;
  usage: { inputTokens: number; outputTokens: number };
}

export interface ModelClient {
  readonly name: string;
  complete(messages: ModelMessage[], tools: Record<string, unknown>[]): Promise<AssistantTurn>;
}
