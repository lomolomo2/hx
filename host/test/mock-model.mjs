// 一个最小的 OpenAI 兼容端点，用来在没有真模型的情况下把整条 CLI 通路跑通。
//
//   node test/mock-model.mjs            # 监听 127.0.0.1:4399
//   PORT=5000 node test/mock-model.mjs
//
// ★ 它替换掉的只有「下一步做什么」这个决策 —— 而那本来就是模型的职责。
//   hx-host 的其余部分完全走真实路径：装配上下文、构造提示词、发 HTTP、
//   解析 tool_calls、过策略与审批、经 hxp 调引擎、把工具结果喂回去。
//   所以这不是"假装成功"，而是把不确定的那一环换成确定的，
//   剩下的部分该坏还是会坏。
//
// 剧本固定：列计划 -> 跑测试(失败) -> 读文件 -> 打补丁 -> 再跑(通过) -> 收尾。
// 配套夹具见 test/fixtures.ts（Windows 用 PowerShell，Linux 用 Python）。
import { createServer } from "node:http";

const PORT = Number(process.env.PORT ?? 4399);
const isWindows = process.platform === "win32";

const SOURCE = isWindows ? "calc.ps1" : "calc.py";

const RUN = isWindows
  ? "powershell -NoProfile -ExecutionPolicy Bypass -File test_calc.ps1"
  : "python3 -B test_calc.py";

const PATCH = isWindows
  ? [
      "*** Begin Patch",
      "*** Update File: calc.ps1",
      "@@",
      " function Add-Values($a, $b) {",
      "-    return $a - $b",
      "+    return $a + $b",
      " }",
      "*** End Patch",
      "",
    ].join("\n")
  : [
      "*** Begin Patch",
      "*** Update File: calc.py",
      "@@",
      " def add(a, b):",
      "-    return a - b",
      "+    return a + b",
      "*** End Patch",
      "",
    ].join("\n");

/** 历史里已经有几轮 assistant，就是走到第几步了。 */
function step(messages) {
  return messages.filter((m) => m.role === "assistant").length;
}

function toolCall(id, name, args) {
  return {
    choices: [
      {
        finish_reason: "tool_calls",
        message: {
          content: null,
          tool_calls: [
            { id, type: "function", function: { name, arguments: JSON.stringify(args) } },
          ],
        },
      },
    ],
    usage: { prompt_tokens: 800, completion_tokens: 40 },
  };
}

function final(text) {
  return {
    choices: [{ finish_reason: "stop", message: { content: text, tool_calls: [] } }],
    usage: { prompt_tokens: 900, completion_tokens: 60 },
  };
}

const server = createServer((req, res) => {
  let body = "";
  req.on("data", (c) => (body += c));
  req.on("end", () => {
    let parsed = {};
    try {
      parsed = JSON.parse(body || "{}");
    } catch {
      /* 空请求也给个回复，免得客户端挂住 */
    }
    const n = step(parsed.messages ?? []);
    let out;
    switch (n) {
      case 0:
        out = toolCall("c0", "todo", {
          items: [
            { text: "run the failing test", completed: false },
            { text: `fix ${SOURCE}`, completed: false },
          ],
        });
        break;
      case 1:
        out = toolCall("c1", "bash", { cmd: RUN });
        break;
      case 2:
        out = toolCall("c2", "read", { path: SOURCE });
        break;
      case 3:
        out = toolCall("c3", "apply_patch", { patch: PATCH });
        break;
      case 4:
        out = toolCall("c4", "bash", { cmd: RUN });
        break;
      case 5:
        out = toolCall("c5", "todo", {
          items: [
            { text: "run the failing test", completed: true },
            { text: `fix ${SOURCE}`, completed: true },
          ],
        });
        break;
      default:
        out = final(
          `The add function was subtracting instead of adding. Fixed it in ${SOURCE}; the test passes now.`,
        );
    }
    process.stderr.write(`[mock] step=${n} -> ${out.choices[0].finish_reason}\n`);
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify(out));
  });
});

server.listen(PORT, "127.0.0.1", () => {
  process.stderr.write(`[mock] listening on http://127.0.0.1:${PORT}/v1\n`);
});
