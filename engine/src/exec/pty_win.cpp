#ifdef _WIN32
#include "exec/pty.hpp"

#include <windows.h>

#include "exec/spawn.hpp"
#include "io/io_win.hpp"
#include "platform/win_util.hpp"

namespace hx {

// Launch 在 spawn_win.cpp 里，pty 与管道两条路径共用它 ——
// 这与 Linux 侧「安全策略仍然走 spawn.cpp 那条路」是同一个安排：
// 沙箱、Job、限额的施加只有一处实现，pty 只负责把两端接好。
namespace win_spawn {
bool Launch(const std::vector<std::string>& argv, const std::string& cwd,
            const std::vector<std::string>& envp, const Confinement* conf, const Limits& limits,
            HANDLE child_in, HANDLE child_out, HANDLE child_err, HPCON pcon, Proc* proc,
            std::string* error);
}  // namespace win_spawn

namespace {

using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

/**
 * ★ 动态解析而不是直接调用。
 *
 *   ConPTY 是 Windows 10 1809（build 17763）才有的。静态链接的话，
 *   在更老的系统上整个 hxd.exe **加载不起来** —— 连 --self-test 都跑不了，
 *   于是 caps 里那句「pty: available=false」永远没机会被打印出来。
 *   探测要有意义，就不能让它的失败模式是「程序起不来」。
 */
CreatePseudoConsoleFn GetCreate() {
  static CreatePseudoConsoleFn fn = [] {
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    return k32 == nullptr ? nullptr
                          : reinterpret_cast<CreatePseudoConsoleFn>(reinterpret_cast<void*>(
                                ::GetProcAddress(k32, "CreatePseudoConsole")));
  }();
  return fn;
}

ClosePseudoConsoleFn GetClose() {
  static ClosePseudoConsoleFn fn = [] {
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    return k32 == nullptr ? nullptr
                          : reinterpret_cast<ClosePseudoConsoleFn>(reinterpret_cast<void*>(
                                ::GetProcAddress(k32, "ClosePseudoConsole")));
  }();
  return fn;
}

}  // namespace

void ClosePtyHandle(void* pcon) {
  if (pcon == nullptr) return;
  ClosePseudoConsoleFn fn = GetClose();
  if (fn != nullptr) fn(static_cast<HPCON>(pcon));
}

PtySpawnResult SpawnPty(const PtySpawnRequest& req) {
  PtySpawnResult r;
  if (req.argv.empty()) {
    r.error = "empty argv";
    return r;
  }
  CreatePseudoConsoleFn create = GetCreate();
  if (create == nullptr) {
    // 如实报「这台机器没有」，而不是悄悄退回管道模式 ——
    // 退回去的话 REPL 会卡在等 tty 上，症状比直接报错难查十倍。
    r.error = "ConPTY unavailable: requires Windows 10 1809 or later";
    return r;
  }

  // ConPTY 要调用方自己给两条管道：一条它读（子进程的输入），一条它写（输出）。
  // 我们这一侧都要 overlapped 才能进事件循环，所以复用 CreatePipePair：
  //   parent_reads=false -> 父端可写（喂输入），子端同步（交给 ConPTY 读）
  //   parent_reads=true  -> 父端可读（收输出），子端同步（交给 ConPTY 写）
  io::Fd in_fd = io::kInvalid, out_fd = io::kInvalid;
  HANDLE conpty_reads = INVALID_HANDLE_VALUE;   // ConPTY 从这里读子进程的输入
  HANDLE conpty_writes = INVALID_HANDLE_VALUE;  // ConPTY 往这里写子进程的输出

  auto cleanup = [&] {
    if (in_fd != io::kInvalid) io::Close(in_fd);
    if (out_fd != io::kInvalid) io::Close(out_fd);
    if (conpty_reads != INVALID_HANDLE_VALUE) ::CloseHandle(conpty_reads);
    if (conpty_writes != INVALID_HANDLE_VALUE) ::CloseHandle(conpty_writes);
  };

  if (!io::win::CreatePipePair(/*parent_reads=*/false, &in_fd, &conpty_reads, &r.error) ||
      !io::win::CreatePipePair(/*parent_reads=*/true, &out_fd, &conpty_writes, &r.error)) {
    cleanup();
    return r;
  }

  COORD size{};
  size.X = static_cast<SHORT>(req.cols);
  size.Y = static_cast<SHORT>(req.rows);
  HPCON pcon = nullptr;
  const HRESULT hr = create(size, conpty_reads, conpty_writes, 0, &pcon);
  if (FAILED(hr) || pcon == nullptr) {
    r.error = "CreatePseudoConsole: " + win::ErrorMessage(static_cast<DWORD>(hr));
    cleanup();
    return r;
  }

  Proc proc;
  const bool ok = win_spawn::Launch(req.argv, req.cwd, req.envp, req.conf, req.limits,
                                    INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
                                    INVALID_HANDLE_VALUE, pcon, &proc, &r.error);

  // ★ 无论成败都要关掉我们手里的这两端。
  //   ConPTY 已经各自复制了一份；我们不关的话，outputWrite 这一侧永远有
  //   持有者，子进程退出后父端读不到 EOF —— cell 就永远不会 done。
  //   这与 Linux 侧 fork 之后父进程必须关掉管道对端是同一个道理。
  ::CloseHandle(conpty_reads);
  conpty_reads = INVALID_HANDLE_VALUE;
  ::CloseHandle(conpty_writes);
  conpty_writes = INVALID_HANDLE_VALUE;

  if (!ok) {
    ClosePtyHandle(pcon);
    cleanup();
    return r;
  }

  proc.pty = pcon;  // 生命周期交给 ReleaseProc：进程回收之后才关
  r.ok = true;
  r.proc = proc;
  r.master_out = out_fd;
  r.master_in = in_fd;
  return r;
}

}  // namespace hx
#endif  // _WIN32
