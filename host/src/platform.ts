// 宿主侧的平台差异，集中在这一个文件。
//
// ★ 这里**不能**碰文件系统或起进程（test/arch.sh 会拦）。所以它只回答
//   "形状"问题：引擎二进制该叫什么名字、命令交给哪个 shell、两个路径
//   算不算在同一棵树下。真要落到磁盘上的动作一律走 hxp 交给引擎。
//
//   node:process 与 node:path 是纯计算，不构成副作用，因此不在禁用之列。
import { platform } from "node:process";
import { fileURLToPath } from "node:url";

export const isWindows = platform === "win32";

/**
 * 引擎二进制的默认路径。
 *
 * ★ 两处坑都在这一行里：
 *   ① Windows 上要 .exe，否则 spawn 直接 ENOENT。
 *   ② 不能用 `new URL(...).pathname`。Windows 上它给出的是
 *      "/H:/myproject/hx/engine/build/hxd.exe" —— 多一个前导斜杠，
 *      spawn 同样 ENOENT，而报错信息看起来路径明明是对的。
 *      fileURLToPath 才是把 file: URL 还原成本地路径的正确做法。
 */
export function defaultEnginePath(relDir: string, baseUrl: string): string {
  const dir = new URL(relDir, baseUrl);
  return fileURLToPath(new URL(isWindows ? "hxd.exe" : "hxd", dir));
}

/**
 * 把一条 shell 命令包成引擎能执行的 argv。
 *
 * ★ Linux 侧用 `bash -c` 而不是 `bash -lc`：登录 shell 会读 ~/.profile，
 *   而 $HOME 在沙箱里不可读，于是每条命令都带一行 "Permission denied" 噪音。
 *
 * ★ Windows 上默认 `cmd /c`。这不是随便挑的：
 *   PowerShell 启动要几百毫秒、会加载用户 profile（沙箱读不到），
 *   而 Git Bash 不一定装了 —— 而且宿主没有 fs，探测不了它在不在。
 *   需要别的 shell 就显式给 HX_SHELL，这比让宿主去猜要诚实。
 */
export function shellCommand(cmd: string, shellOverride?: string): string[] {
  const shell = shellOverride?.trim();
  if (shell) {
    // "bash" / "pwsh" / "C:\\Program Files\\Git\\bin\\bash.exe" 都接受
    const lower = shell.toLowerCase();
    if (lower.endsWith("cmd") || lower.endsWith("cmd.exe")) return [shell, "/c", cmd];
    if (lower.includes("powershell") || lower.includes("pwsh")) {
      // ★ PowerShell 在沙箱里必须先挂一个 PSDrive，否则相对路径全指到 C:\。
      //
      //   症状：进程 cwd 是对的（[Environment]::CurrentDirectory 没问题，
      //   .NET 的相对路径也没问题），但 PowerShell 的 **provider location**
      //   退回了 C:\ —— 于是 Get-ChildItem / Get-Content 这些 cmdlet 的
      //   相对路径指向错误的地方。同一个进程里两套相对路径语义，极难查。
      //
      //   原因：PowerShell 切目录时要访问目标的**父目录**，而沙箱只授权了
      //   工作区本身（父链不能授权 —— 在 C:\Users\<user> 上改 ACL 会触发
      //   整个 profile 的继承重算，实测十分钟没回来）。
      //
      //   `Set-Location <绝对路径>` 同样失败（Access is denied）。
      //   实测可行的是 New-PSDrive：它直接以工作区为 root 建一个驱动器，
      //   不需要走父链。挂上之后 Set-Location hx: 就成立了。
      const fix =
        "New-PSDrive -Name hx -PSProvider FileSystem " +
        "-Root ([Environment]::CurrentDirectory) -Scope Global | Out-Null; " +
        "Set-Location hx:; ";
      return [shell, "-NoProfile", "-NonInteractive", "-Command", fix + cmd];
    }
    return [shell, "-c", cmd];
  }
  return isWindows ? ["cmd", "/c", cmd] : ["bash", "-c", cmd];
}

/** 给模型看的 shell 名字。说错了，它会一直发另一种方言的命令。 */
export function shellName(shellOverride?: string): string {
  const shell = shellOverride?.trim();
  if (shell) return shell;
  return isWindows ? "cmd.exe" : "bash";
}

/**
 * path 是否落在 root 之内（按路径分量边界判断）。
 *
 * ★ Windows 上有两处非比不可的差异，漏掉任何一处都是**权限判断出错**，
 *   而不是"显示问题"：
 *     ① 分隔符可能是 '/' 也可能是 '\\'，同一个目录能写成两种样子
 *     ② NTFS 默认大小写不敏感，"C:\\Repo" 与 "c:\\repo" 是同一个目录
 *   按字节比的话，子 agent 只要把 root 换个大小写写一遍，
 *   intersect 就会认为那是一个"新的 root"而不是父 root 的子集。
 */
export function pathWithin(path: string, root: string): boolean {
  const norm = (p: string): string => {
    let s = isWindows ? p.replace(/\//g, "\\") : p;
    if (isWindows) s = s.toLowerCase();
    // 去掉尾部分隔符，让 "C:\\a\\" 与 "C:\\a" 等价（根目录 "C:\\" / "/" 除外）
    while (s.length > 1 && (s.endsWith("/") || s.endsWith("\\"))) {
      if (isWindows && s.length === 3 && s[1] === ":") break;
      s = s.slice(0, -1);
    }
    return s;
  };
  const p = norm(path);
  const r = norm(root);
  if (p === r) return true;
  const sep = isWindows ? "\\" : "/";
  return p.startsWith(r.endsWith(sep) ? r : `${r}${sep}`);
}
