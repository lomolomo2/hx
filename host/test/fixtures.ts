// 测试夹具里那些**必须随平台变**的东西。
//
// ★ 为什么不统一用一种语言：沙箱里跑得起来的解释器，两个平台不一样。
//   两边挑的都是「装在系统目录、因而在沙箱里天然可见」的那一个 ——
//   这不是将就，是同一条原则在两个平台上的结果。
//
//   Linux  用 python3 —— 它在 /usr/bin，属于默认只读路径。
//          不能用 node：nvm 装的 node 在 $HOME 下，而 $HOME 默认不可读
//          （README「已知的坑」第一条），测试会因为环境不同而时好时坏。
//
//   Windows 用 powershell.exe —— 它在 %SystemRoot%\System32 下，
//          出厂就授权给 ALL APPLICATION PACKAGES。
//
//   Windows 上被排除掉的那几个，实测结果记在这里，省得下次再试一遍：
//     · python3 —— 是个 Microsoft Store 占位程序，跑起来只提示去商店安装；
//       真正的 python.exe 装在 %LOCALAPPDATA%，沙箱读不到
//     · node    —— 装在 Program Files，但 Node 安装程序**断开了 ACL 继承**，
//       那个目录没有 ALL APPLICATION PACKAGES 的 ACE，AppContainer 起不来
//     · git / cscript —— 同样起不来
//
//   这正是 Windows 版的「$HOME 里的工具链跑不了」，而且更麻烦一点：
//   连 Program Files 里的东西都可能中招，而补 ACL 需要管理员权限。
//   判据是目标目录有没有给 ALL APPLICATION PACKAGES 授权，
//   `icacls <dir>` 一看便知。
import { isWindows } from "../src/platform.js";

export interface Fixture {
  /** 被修的源文件 */
  sourceName: string;
  sourceBroken: string;
  /** 跑测试的文件 */
  testName: string;
  testBody: string;
  /** 跑测试的命令（交给 bash 工具，由 platform.shellCommand 包装） */
  runCommand: string;
  /** 把 source 从"坏"改成"好"的补丁 */
  patch: string;
  /** 修好之后源文件里应该出现的内容 */
  fixedMarker: string;
}

const python: Fixture = {
  sourceName: "calc.py",
  sourceBroken: "def add(a, b):\n    return a - b\n",
  testName: "test_calc.py",
  testBody: 'from calc import add\nassert add(2, 3) == 5\nprint("ALL TESTS PASS")\n',
  // -B：不写 .pyc。补丁把 "a - b" 改成 "a + b" 字节数不变，若又落在同一秒内，
  // Python 的 (mtime秒, size) 校验会判定缓存有效 —— 改对了却仍报失败。
  runCommand: "python3 -B test_calc.py",
  patch: `*** Begin Patch
*** Update File: calc.py
@@
 def add(a, b):
-    return a - b
+    return a + b
*** End Patch
`,
  fixedMarker: "return a + b",
};

const powershell: Fixture = {
  sourceName: "calc.ps1",
  sourceBroken: "function Add-Values($a, $b) {\r\n    return $a - $b\r\n}\r\n",
  testName: "test_calc.ps1",
  // 三处都是实测踩出来的：
  //   · $ErrorActionPreference = Stop —— 否则 Write-Error 不中断执行，
  //     脚本带着一屏报错照样 exit 0，于是"测试通过"了但什么都没验到
  //   · $PSScriptRoot —— 写 ".\calc.ps1" 点源会 CommandNotFound：
  //     PowerShell 的 provider 当前位置未必等于进程 cwd
  //   · -ExecutionPolicy Bypass 放在命令行上 —— 默认策略会拒绝执行 .ps1 文件，
  //     那是本机配置，不该让端到端测试的成败取决于它
  testBody:
    "$ErrorActionPreference = 'Stop'\r\n" +
    '. "$PSScriptRoot\\calc.ps1"\r\n' +
    "$r = Add-Values 2 3\r\n" +
    "if ($r -ne 5) { Write-Output \"FAIL: got $r\"; exit 1 }\r\n" +
    "Write-Output 'ALL TESTS PASS'\r\n",
  runCommand: "powershell -NoProfile -ExecutionPolicy Bypass -File test_calc.ps1",
  // ★ 补丁本身是 LF 的，而夹具文件是 CRLF —— 这是故意的：
  //   它顺带验了 apply_patch 的行尾处理（见 fs/apply_patch.cpp 的 LineEnding）。
  //   不处理 CRLF 的话，这个补丁会报 "context not found"。
  patch: `*** Begin Patch
*** Update File: calc.ps1
@@
 function Add-Values($a, $b) {
-    return $a - $b
+    return $a + $b
 }
*** End Patch
`,
  fixedMarker: "return $a + $b",
};

export const fixture: Fixture = isWindows ? powershell : python;

/** 产出大量输出、用来把上下文顶到压缩阈值的命令。 */
export function fillerCommand(step: number): string {
  return isWindows
    ? `powershell -NoProfile -Command "Write-Output ('STEP${step} filler line. ' * 300)"`
    : `python3 -c "print('STEP${step} filler line. ' * 300)"`;
}
