// ★ 架构约束的强制点。
//
// host 决定"该不该做"，hxd 决定"能不能做"。如果 host 能直接碰文件系统
// 或起进程，那条边界就只是口头约定 —— 这条规则让它变成 lint 错误。
//
// 唯一豁免：src/engine/client.ts（它负责把引擎拉起来，文件内有 eslint-disable）。
module.exports = {
  root: true,
  parser: "@typescript-eslint/parser",
  parserOptions: { ecmaVersion: 2022, sourceType: "module" },
  rules: {
    "no-restricted-imports": [
      "error",
      {
        paths: [
          { name: "node:fs", message: "host 不得直接碰文件系统：改用 EngineClient.fsRead / applyPatch" },
          { name: "fs", message: "host 不得直接碰文件系统：改用 EngineClient.fsRead / applyPatch" },
          { name: "node:fs/promises", message: "host 不得直接碰文件系统：改用 EngineClient" },
          { name: "node:child_process", message: "host 不得起进程：改用 EngineClient.execStart（仅 engine/client.ts 豁免）" },
          { name: "child_process", message: "host 不得起进程：改用 EngineClient.execStart" },
        ],
      },
    ],
  },
};
