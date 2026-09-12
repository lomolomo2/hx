// A minimal OpenAI-compatible endpoint, for exercising the whole CLI path
// without a real model.
//
//   node test/mock-model.mjs            # listens on 127.0.0.1:4399
//   PORT=5000 node test/mock-model.mjs
//
// ★ The only thing it replaces is the decision of "what to do next" -- which
//   is precisely the model's job. Every other part of hx-host takes the real
//   path: assembling context, building the prompt, sending HTTP, parsing
//   tool_calls, going through policy and approval, calling the engine over
//   hxp, and feeding tool results back.
//   So this is not "faking success"; it swaps the one uncertain link for a
//   deterministic one, and everything else still breaks when it is broken.
//
// The script is fixed: make a plan -> run the tests (fail) -> read the file ->
// apply a patch -> run again (pass) -> wrap up.
// The matching fixtures are in test/fixtures.ts (PowerShell on Windows, Python
// on Linux).
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

/** However many assistant turns the history already holds is the step we are on. */
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
      /* answer even an empty request, so the client does not hang */
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
