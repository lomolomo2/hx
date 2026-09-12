#ifdef _WIN32
#include "exec/spawn.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <vector>

#include "io/io_win.hpp"
#include "platform/platform.hpp"
#include "platform/win_util.hpp"
#include "sandbox/confine_win.hpp"

namespace hx {
namespace win_spawn {

/**
 * argv -> 命令行字符串。
 *
 * ★ Windows 没有 execve(argv[])：CreateProcess 只收一个字符串，
 *   由子进程自己（通常用 CommandLineToArgvW）再拆回来。也就是说
 *   **引号规则是这一层的安全边界**。拼错了，argv 里一个带空格或引号的
 *   文件名就会被拆成两个参数 —— 那正是 grep.ts 注释里担心的那类注入口子，
 *   只不过发生在 Windows 侧而不是 shell 里。
 *
 * 规则（与 CommandLineToArgvW 严格互逆）：
 *   · 含空格 / 制表符 / 引号 / 为空 -> 整体加引号
 *   · 引号前的反斜杠要成对加倍
 *   · 内嵌引号前再加一个反斜杠
 */
std::wstring ArgvToCommandLine(const std::vector<std::string>& argv) {
  std::wstring out;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i > 0) out.push_back(L' ');
    const std::wstring a = win::Widen(argv[i]);
    const bool need_quote =
        a.empty() || a.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!need_quote) {
      out += a;
      continue;
    }
    out.push_back(L'"');
    for (size_t j = 0; j < a.size(); ++j) {
      size_t slashes = 0;
      while (j < a.size() && a[j] == L'\\') {
        ++slashes;
        ++j;
      }
      if (j == a.size()) {
        out.append(slashes * 2, L'\\');  // 结尾的反斜杠要加倍，否则会转义收尾引号
        break;
      }
      if (a[j] == L'"') {
        out.append(slashes * 2 + 1, L'\\');
      } else {
        out.append(slashes, L'\\');
      }
      out.push_back(a[j]);
    }
    out.push_back(L'"');
  }
  return out;
}

/**
 * cmd.exe 的命令行要另外拼，**不能**用 ArgvToCommandLine。
 *
 * ★ 这是 Windows 上最阴的一条规则差异：
 *   CreateProcess 传的是一整条字符串，绝大多数程序用 CommandLineToArgvW
 *   把它拆回 argv —— 那套规则里内嵌引号写成 \"。但 **cmd.exe 不用那套规则**：
 *   它把反斜杠当普通字符，引号只做简单配对。
 *
 *   于是 ["cmd","/c","powershell -Command \"...\""] 按通用规则拼出来，
 *   cmd 看到的是一堆字面的 \" ，传给 powershell 的命令变成了一个**字符串
 *   字面量**。实测现象是 powershell 把命令原文回显出来，退出码 0，
 *   看起来"跑成功了"，只是什么都没做 —— 又一个静默失败。
 *
 *   正确做法是 `/s`：加上它之后，cmd 只把 /c 后面的第一个和最后一个引号
 *   剥掉，中间原样不动。于是我们只要把命令整体包一层引号，
 *   内部引号一个都不用转义。
 */
bool IsCmdExe(const std::wstring& exe) {
  size_t slash = exe.find_last_of(L"\\/");
  std::wstring name = (slash == std::wstring::npos) ? exe : exe.substr(slash + 1);
  for (wchar_t& c : name) c = static_cast<wchar_t>(::towlower(c));
  return name == L"cmd.exe";
}

/** 返回 true 表示已按 cmd.exe 的规则拼好；false 表示该走通用路径。 */
bool BuildCmdExeCommandLine(const std::wstring& exe, const std::vector<std::string>& argv,
                            std::wstring* out) {
  if (!IsCmdExe(exe) || argv.size() < 3) return false;
  std::wstring sw = win::Widen(argv[1]);
  for (wchar_t& c : sw) c = static_cast<wchar_t>(::towlower(c));
  if (sw != L"/c" && sw != L"/k") return false;

  std::wstring body;
  for (size_t i = 2; i < argv.size(); ++i) {
    if (i > 2) body.push_back(L' ');
    body += win::Widen(argv[i]);
  }
  // cmd 会剥掉最外层的一对引号；命令自己末尾带引号时会误判，补一个空格隔开。
  if (!body.empty() && body.back() == L'"') body.push_back(L' ');

  *out = L"cmd.exe /s " + sw + L" \"" + body + L"\"";
  return true;
}

/** "K=V" 列表 -> CreateProcess 的环境块（\0 分隔，末尾双 \0）。 */
std::wstring BuildEnvBlock(const std::vector<std::string>& envp) {
  // Windows 要求环境块按变量名不区分大小写排序。不排在多数情况下也能跑，
  // 但个别运行时（含一些 MSVC 工具）会二分查找，乱序时表现为「变量明明设了却读不到」。
  std::vector<std::wstring> items;
  items.reserve(envp.size());
  for (const auto& e : envp) items.push_back(win::Widen(e));
  std::sort(items.begin(), items.end(), [](const std::wstring& a, const std::wstring& b) {
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  std::wstring block;
  for (const auto& it : items) {
    block += it;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');  // 空环境时也必须是双 \0
  return block;
}

/** 从 envp 里取 PATH（大小写不敏感）。 */
std::wstring PathFromEnv(const std::vector<std::string>& envp) {
  for (const auto& e : envp) {
    const size_t eq = e.find('=');
    if (eq == std::string::npos) continue;
    std::string k = e.substr(0, eq);
    for (char& c : k) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (k == "path") return win::Widen(e.substr(eq + 1));
  }
  return std::wstring();
}

/**
 * 解析可执行文件的绝对路径。
 *
 * ★ 为什么不把 lpApplicationName 留空让 CreateProcess 自己找：
 *   它用的是**调用方**（hxd）的 PATH，而我们刻意给子进程换了一份最小环境。
 *   留空的话 envp 里的 PATH 形同虚设 —— 沙箱里跑的是宿主 PATH 上的程序，
 *   与策略里写的那份不是一回事。自己用 envp 的 PATH 查，才对得上。
 */
bool ResolveExecutable(const std::string& prog, const std::vector<std::string>& envp,
                       const std::string& cwd, std::wstring* out) {
  const std::wstring wprog = win::Widen(prog);
  const std::wstring path = PathFromEnv(envp);

  // 带分隔符的当相对/绝对路径解析，不查 PATH（与 execvp 的行为一致）
  const bool has_sep = prog.find('/') != std::string::npos || prog.find('\\') != std::string::npos;

  // 带分隔符时在 cwd 下解析，否则沿 envp 的 PATH 查。
  const std::wstring wcwd = win::Widen(cwd);
  const wchar_t* search_root = has_sep ? (wcwd.empty() ? nullptr : wcwd.c_str())
                                       : (path.empty() ? nullptr : path.c_str());

  // ★ 绝不能先试「不补扩展名」。
  //
  //   POSIX 的 execvp 找的就是无扩展名的可执行文件，照搬过来是错的：
  //   Windows 上无扩展名的文件根本不可执行，而 PATH 上常常躺着同名的
  //   shell 脚本（MSYS/Git-Bash 就在 bin 下放了一堆）。实测 "cmd" 会先
  //   命中 C:\MinGW\msys\1.0\bin\cmd 这个脚本，而不是 System32\cmd.exe。
  //
  //   SearchPathW 的 lpExtension 只在「文件名本身没有扩展名」时才追加，
  //   所以按 PATHEXT 顺序依次传就同时覆盖了两种写法：
  //     "cmd"      -> 补成 cmd.exe
  //     "node.exe" -> 已有扩展名，原样查
  const wchar_t* kExts[] = {L".exe", L".com", L".bat", L".cmd"};
  std::wstring buf(MAX_PATH, L'\0');
  for (const wchar_t* ext : kExts) {
    DWORD n = ::SearchPathW(search_root, wprog.c_str(), ext, static_cast<DWORD>(buf.size()),
                            buf.data(), nullptr);
    if (n >= buf.size()) {  // 缓冲不够，n 是需要的长度
      buf.assign(static_cast<size_t>(n) + 1, L'\0');
      n = ::SearchPathW(search_root, wprog.c_str(), ext, static_cast<DWORD>(buf.size()),
                        buf.data(), nullptr);
    }
    if (n > 0 && n < buf.size()) {
      buf.resize(n);
      *out = buf;
      return true;
    }
    buf.assign(MAX_PATH, L'\0');
  }
  return false;
}

/** CreateProcess 的公共部分。成功时填好 proc。 */
bool Launch(const std::vector<std::string>& argv, const std::string& cwd,
            const std::vector<std::string>& envp, const Confinement* conf, const Limits& limits,
            HANDLE child_in, HANDLE child_out, HANDLE child_err, HPCON pcon, Proc* proc,
            std::string* error) {
  if (argv.empty()) {
    *error = "empty argv";
    return false;
  }

  std::wstring exe;
  if (!ResolveExecutable(argv[0], envp, cwd, &exe)) {
    *error = "executable not found on PATH: " + argv[0];
    return false;
  }
  std::wstring cmdline;
  // cmd.exe 的引号规则与 CommandLineToArgvW 不同，必须另拼 —— 见 BuildCmdExeCommandLine
  if (!BuildCmdExeCommandLine(exe, argv, &cmdline)) cmdline = ArgvToCommandLine(argv);
  std::wstring envblock = BuildEnvBlock(envp);
  // HX_DEBUG_SPAWN=1 时把真正交给 CreateProcess 的东西原样打出来。
  // 排查「命令在壳里能跑、在沙箱里不行」这类问题时，第一件事就是确认
  // exe / cmdline / env 三者到底是什么 —— 猜是最慢的办法。
  if (std::string dbg; platform::GetEnv("HX_DEBUG_SPAWN", &dbg)) {
    std::fprintf(stderr, "hxd/debug: exe=[%s]\n", win::Narrow(exe).c_str());
    std::fprintf(stderr, "hxd/debug: cmdline=[%s]\n", win::Narrow(cmdline).c_str());
    std::fprintf(stderr, "hxd/debug: cwd=[%s]\n", cwd.c_str());
    for (const auto& e : envp) std::fprintf(stderr, "hxd/debug: env [%s]\n", e.c_str());
    std::fprintf(stderr, "hxd/debug: envblock wchars=%zu\n", envblock.size());
  }
  const std::wstring wcwd = win::Widen(cwd);

  // ---- Job：先建好、先装限额，进程一进来就已经被圈住 ----
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    *error = win::LastError("CreateJobObject");
    return false;
  }
  if (!ApplyLimitsToJob(job, limits, error)) {
    ::CloseHandle(job);
    return false;
  }

  // ---- 进程属性表 ----
  SIZE_T attr_size = 0;
  // 属性数：安全能力（有沙箱时）、句柄白名单（非 pty 时）、伪控制台（pty 时）
  DWORD attr_count = 1;
  if (conf != nullptr) ++attr_count;
  ::InitializeProcThreadAttributeList(nullptr, attr_count, 0, &attr_size);
  std::vector<char> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  if (::InitializeProcThreadAttributeList(attrs, attr_count, 0, &attr_size) == 0) {
    *error = win::LastError("InitializeProcThreadAttributeList");
    ::CloseHandle(job);
    return false;
  }
  auto attr_guard = std::unique_ptr<void, void (*)(void*)>(
      attrs, [](void* p) { ::DeleteProcThreadAttributeList(
                  reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(p)); });

  SECURITY_CAPABILITIES sec{};
  if (conf != nullptr) {
    // ★ 这一步就是 Windows 上的 landlock_restrict_self。
    //   差别在时机：Linux 是子进程 fork 之后自己降权（所以顺序错了会留窗口期），
    //   Windows 是父进程在建进程时把 token 定死，子进程连第一条指令都还没执行。
    //   没有窗口期可言，也就没有「顺序错了等于没做」这个坑。
    if (!FillSecurityCapabilities(*conf, &sec)) {
      *error = "FillSecurityCapabilities failed";
      ::CloseHandle(job);
      return false;
    }
    if (::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sec,
                                    sizeof(sec), nullptr, nullptr) == 0) {
      *error = win::LastError("UpdateProcThreadAttribute(security capabilities)");
      ::CloseHandle(job);
      return false;
    }
  }

  HANDLE inherit[3];
  DWORD inherit_n = 0;
  if (pcon != nullptr) {
    // pty 路径：伪控制台自带 stdio，不走句柄白名单
    if (::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pcon,
                                    sizeof(pcon), nullptr, nullptr) == 0) {
      *error = win::LastError("UpdateProcThreadAttribute(pseudoconsole)");
      ::CloseHandle(job);
      return false;
    }
  } else {
    // ★ 句柄白名单 = O_CLOEXEC 的对位。
    //   CreateProcess 的 bInheritHandles=TRUE 会把**所有**可继承句柄一起给出去。
    //   不显式列白名单的话，另一个 cell 的管道端就可能漏进这个子进程 ——
    //   两个本该互不可见的沙箱之间凭空多出一条通道。
    if (child_in != INVALID_HANDLE_VALUE) inherit[inherit_n++] = child_in;
    if (child_out != INVALID_HANDLE_VALUE) inherit[inherit_n++] = child_out;
    if (child_err != INVALID_HANDLE_VALUE) inherit[inherit_n++] = child_err;
    if (inherit_n > 0 &&
        ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                    inherit_n * sizeof(HANDLE), nullptr, nullptr) == 0) {
      *error = win::LastError("UpdateProcThreadAttribute(handle list)");
      ::CloseHandle(job);
      return false;
    }
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.lpAttributeList = attrs;
  if (pcon == nullptr) {
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = child_err;
  }

  PROCESS_INFORMATION pi{};
  DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                // ★ 挂起着建。AssignProcessToJobObject 必须赶在子进程跑起来之前，
                //   否则它有机会在入 Job 前 fork 出逃出限额的孙进程。
                CREATE_SUSPENDED |
                // 自成进程组：这是 SignalGroup 能送 CTRL_BREAK 的前提，
                // 也让子进程不会收到我们这个控制台的 Ctrl+C。
                CREATE_NEW_PROCESS_GROUP;
  // ★ 必须是 CREATE_NO_WINDOW，**不能**用 DETACHED_PROCESS。这里踩过一次，值得写全。
  //
  //   两者都不弹窗，区别是要不要给子进程配一个控制台。配了控制台，系统会
  //   顺带塞一个 conhost.exe 进我们的 Job —— 它在客户端退出后还活一小会儿，
  //   于是每条普通命令都被判成「留下了孤儿进程」。为了躲开这个，
  //   一度改成了 DETACHED_PROCESS（反正三条 stdio 都重定向了，看似不需要控制台）。
  //
  //   结果是一次**静默**的塌方：cmd.exe 自己照样跑，echo / type 这些**内建**
  //   命令也照常有输出，但 cmd 再去起的任何**外部**程序（node / ping /
  //   powershell）全部拿不到 stdout —— 退出码 0 或 1，一个字节都不输出。
  //   也就是说「跑一条命令」看着是好的，「跑一条真实的构建命令」全废。
  //   这正是 README 那条教训的翻版：冒烟用例恰好只用了内建命令，于是
  //   套件全绿而实际工作全灭。
  //
  //   conhost 的问题改在别处解决：GroupAlive 按镜像路径把它排除掉
  //   （见 exec/proc_win.cpp），那里既精确，又不碰任何执行路径。
  if (pcon == nullptr) flags |= CREATE_NO_WINDOW;

  // ★ 这里刻意没有任何「关掉沙箱」的调试开关。
  //   诊断开关只该是只读的（HX_DEBUG_SPAWN 只打印）。一个能用环境变量
  //   摘掉 AppContainer 的旁路，本身就是一条提权通道 —— 而且是最容易
  //   被忘在生产二进制里的那种。
  if (::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr,
                       /*bInheritHandles=*/pcon == nullptr ? TRUE : FALSE, flags, envblock.data(),
                       wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi) == 0) {
    const DWORD e = ::GetLastError();
    *error = "CreateProcess(" + argv[0] + "): " + win::ErrorMessage(e);
    ::CloseHandle(job);
    return false;
  }

  if (::AssignProcessToJobObject(job, pi.hProcess) == 0) {
    // 进不了 Job 就不放行 —— 与「装不上沙箱就不执行」是同一条原则：
    // 没有 Job 就没有进程树所有权，一个 `start /b` 就能把后台进程留在系统里。
    *error = win::LastError("AssignProcessToJobObject");
    ::TerminateProcess(pi.hProcess, 126);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(job);
    return false;
  }

  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);

  proc->pid = static_cast<int64_t>(pi.dwProcessId);
  proc->handle = pi.hProcess;
  proc->group = job;
  return true;
}

}  // namespace win_spawn

SpawnCellResult SpawnCell(const SpawnCellRequest& req) {
  SpawnCellResult r;
  if (req.argv.empty()) {
    r.error = "empty argv";
    return r;
  }
  // 沙箱装不上就不执行。这条不变式两个平台完全一致。
  // （conf 为空只在 danger-full-access 下合法，调用方已经判过。）

  io::Fd in_fd = io::kInvalid, out_fd = io::kInvalid, err_fd = io::kInvalid;
  HANDLE child_in = INVALID_HANDLE_VALUE, child_out = INVALID_HANDLE_VALUE,
         child_err = INVALID_HANDLE_VALUE;
  auto cleanup = [&] {
    for (io::Fd f : {in_fd, out_fd, err_fd}) {
      if (f != io::kInvalid) io::Close(f);
    }
    for (HANDLE h : {child_in, child_out, child_err}) {
      if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
  };

  if (!io::win::CreatePipePair(/*parent_reads=*/false, &in_fd, &child_in, &r.error) ||
      !io::win::CreatePipePair(/*parent_reads=*/true, &out_fd, &child_out, &r.error) ||
      !io::win::CreatePipePair(/*parent_reads=*/true, &err_fd, &child_err, &r.error)) {
    cleanup();
    return r;
  }

  if (!win_spawn::Launch(req.argv, req.cwd, req.envp, req.conf, req.limits, child_in, child_out,
                         child_err, nullptr, &r.proc, &r.error)) {
    cleanup();
    return r;
  }

  // 子进程那三端交出去了，父进程立刻关掉自己手里的副本 ——
  // 不关的话子进程退出后管道永远不会 EOF，cell 就永远不会 done。
  ::CloseHandle(child_in);
  ::CloseHandle(child_out);
  ::CloseHandle(child_err);

  r.ok = true;
  r.in_fd = in_fd;
  r.out_fd = out_fd;
  r.err_fd = err_fd;
  return r;
}

SpawnResult RunForeground(const std::vector<std::string>& argv, const std::string& cwd,
                          const std::vector<std::string>& envp, const Confinement* conf,
                          const SeccompPlan& seccomp, const Limits& limits) {
  SpawnResult r;
  (void)seccomp;  // Windows 上没有这一层，见 sandbox/seccomp.hpp 的说明
  if (argv.empty()) {
    r.error = "empty argv";
    return r;
  }

  Proc proc;
  // 前台跑：直接继承 hxd 自己的 stdio，逃逸测试要看到的是真实输出
  if (!win_spawn::Launch(argv, cwd, envp, conf, limits, ::GetStdHandle(STD_INPUT_HANDLE),
                         ::GetStdHandle(STD_OUTPUT_HANDLE), ::GetStdHandle(STD_ERROR_HANDLE),
                         nullptr, &proc, &r.error)) {
    return r;
  }

  ::WaitForSingleObject(proc.handle, INFINITE);
  int code = -1, sig = 0;
  Reap(&proc, &code, &sig);
  r.started = true;
  r.exit_code = code;
  r.term_signal = sig;
  ReleaseProc(&proc);
  return r;
}

}  // namespace hx
#endif  // _WIN32
