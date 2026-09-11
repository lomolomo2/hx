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
  /** 推理模型（Qwen3 / DeepSeek-R1 等）把思考过程单独放在这里。 */
  reasoning?: string;
  toolCalls: ToolCall[];
  finishReason?: string;
  usage: { inputTokens: number; outputTokens: number };
}

export interface ModelClient {
  readonly name: string;
  complete(messages: ModelMessage[], tools: Record<string, unknown>[]): Promise<AssistantTurn>;
}
