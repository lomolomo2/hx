// hx serve —— 把 harness 暴露成 HTTP + SSE。
//   HX_BASE_URL=... HX_MODEL=... npx tsx src/server/main.ts --port 4100
import { serve } from "@hono/node-server";

import { createApp } from "./app.js";
import { OpenAICompatClient } from "../model/openai_compat.js";

const DEFAULT_HXD = new URL("../../../engine/build/hxd", import.meta.url).pathname;

const argv = process.argv.slice(2);
const portIdx = argv.indexOf("--port");
const port = portIdx >= 0 ? Number(argv[portIdx + 1]) : Number(process.env["HX_PORT"] ?? 4100);

const baseUrl = process.env["HX_BASE_URL"];
const model = process.env["HX_MODEL"];
if (!baseUrl || !model) {
  console.error("set HX_BASE_URL and HX_MODEL (any OpenAI-compatible endpoint)");
  process.exit(78);
}

const { app } = createApp({
  enginePath: process.env["HX_ENGINE"] ?? DEFAULT_HXD,
  makeModel: () =>
    new OpenAICompatClient(
      baseUrl,
      process.env["HX_API_KEY"] ?? "",
      model,
      Number(process.env["HX_MAX_TOKENS"] ?? 4096),
    ),
});

serve({ fetch: app.fetch, port, hostname: "127.0.0.1" }, (info) => {
  console.log(`hx serve → http://127.0.0.1:${info.port}`);
  console.log(`  engine: ${process.env["HX_ENGINE"] ?? DEFAULT_HXD}`);
  console.log(`  model : ${model} @ ${baseUrl}`);
});
