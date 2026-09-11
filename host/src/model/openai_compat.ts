// 任何 OpenAI 兼容端点：DeepSeek / 本机 llama.cpp / OpenRouter / vLLM …
// 换后端只是换 baseUrl，不改任何其它代码。
import type { AssistantTurn, ModelClient, ModelMessage } from "./types.js";

interface WireToolCall {
  id: string;
  function: { name: string; arguments: string };
}

export class OpenAICompatClient implements ModelClient {
  readonly name: string;

  constructor(
    private readonly baseUrl: string,
    private readonly apiKey: string,
    private readonly model: string,
    /** 推理模型会先花掉一大截 token 思考，给少了 content 会是空的。 */
    private readonly maxTokens = 4096,
    private readonly temperature?: number,
  ) {
    this.name = `${model} @ ${baseUrl}`;
  }

  async complete(messages: ModelMessage[], tools: Record<string, unknown>[]): Promise<AssistantTurn> {
    const body = {
      model: this.model,
      messages: messages.map((m) => {
        const wire: Record<string, unknown> = { role: m.role, content: m.content };
        if (m.toolCalls?.length) {
          wire["tool_calls"] = m.toolCalls.map((t) => ({
            id: t.id,
            type: "function",
            function: { name: t.name, arguments: t.argumentsJson },
          }));
        }
        if (m.toolCallId) wire["tool_call_id"] = m.toolCallId;
        return wire;
      }),
      max_tokens: this.maxTokens,
      ...(this.temperature !== undefined ? { temperature: this.temperature } : {}),
      ...(tools.length > 0 ? { tools } : {}),
    };

    const res = await fetch(`${this.baseUrl}/chat/completions`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        ...(this.apiKey ? { authorization: `Bearer ${this.apiKey}` } : {}),
      },
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      throw new Error(`model http ${res.status}: ${(await res.text()).slice(0, 300)}`);
    }
    const json = (await res.json()) as {
      choices?: {
        finish_reason?: string;
        message?: { content?: string | null; reasoning_content?: string | null; tool_calls?: WireToolCall[] };
      }[];
      usage?: { prompt_tokens?: number; completion_tokens?: number };
    };
    const choice = json.choices?.[0] ?? {};
    const msg = choice.message ?? {};
    const reasoning = msg.reasoning_content ?? undefined;
    return {
      content: msg.content ?? null,
      ...(reasoning ? { reasoning } : {}),
      toolCalls: (msg.tool_calls ?? []).map((t) => ({
        // 有些服务端不回 id（llama.cpp 会回），兜一个稳定值免得关联不上
        id: t.id || `call_${t.function.name}_${Math.random().toString(36).slice(2, 10)}`,
        name: t.function.name,
        argumentsJson: t.function.arguments,
      })),
      ...(choice.finish_reason ? { finishReason: choice.finish_reason } : {}),
      usage: {
        inputTokens: json.usage?.prompt_tokens ?? 0,
        outputTokens: json.usage?.completion_tokens ?? 0,
      },
    };
  }
}
