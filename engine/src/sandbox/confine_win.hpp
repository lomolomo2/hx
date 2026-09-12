// Windows 的 Confinement 定义：一个每会话唯一的 AppContainer。
//
// ★ 为什么 AppContainer 是 Landlock 的对位，而不是「差不多的东西」：
//
//   Landlock 是 allow-list —— ruleset 里没授予的路径一律拒绝，
//   不需要任何黑名单，符号链接在解析后的路径上判定。
//
//   AppContainer 也是 allow-list —— 进程 token 里带一个**低权限的
//   package SID**，访问任何内核对象时，只有 DACL 里明确授予了这个 SID
//   （或 ALL APPLICATION PACKAGES）的才放行。用户 profile 目录默认
//   两者都不给，所以 `type %USERPROFILE%\.ssh\id_rsa` 直接 ACCESS_DENIED
//   —— 和 README 里 `cat ~/.ssh/id_rsa` 返回 EACCES 是同一个机制，
//   同样不需要黑名单。
//
//   网络那一半更干净：Landlock ABI 4 只管 TCP，UDP/DNS 是盲区，
//   所以 Linux 侧必须再加一层 seccomp 封掉 AF_INET。AppContainer 不需要 ——
//   没有 internetClient 能力时，WFP 在内核里挡掉**全部**出站，
//   TCP 和 UDP 一起。net:deny 在这里是一层就够，而不是两层拼出来的。
//
// ★ 代价要说清楚：授权 roots 是通过在真实目录上加 ACE 实现的，
//   这是对用户文件系统的**持久修改**。所以 ACE 只授予本会话那个唯一的
//   AppContainer SID，并在 Confinement 析构时撤销。进程被 kill -9 时
//   ACE 会残留 —— 见 .cpp 里 kAceTag 的说明。
#pragma once
#ifdef _WIN32

#include <windows.h>

#include <string>
#include <vector>

#include "sandbox/confine.hpp"

namespace hx {

struct Confinement {
  PSID sid = nullptr;             // AppContainer 的 package SID
  std::wstring profile_name;      // 本会话唯一，形如 hx-<pid>-<tick>
  std::vector<PSID> cap_sids;     // 授予的能力（net:allow 时才有 internetClient）
  std::vector<std::wstring> granted_paths;  // 打过 ACE 的路径，析构时撤销
  bool net_allowed = false;

  ~Confinement();
  Confinement() = default;
  Confinement(const Confinement&) = delete;
  Confinement& operator=(const Confinement&) = delete;
};

/**
 * 填出 CreateProcess 用的 SECURITY_CAPABILITIES。
 *
 * 由 spawn_win.cpp / pty_win.cpp 通过
 * PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES 传给 CreateProcess ——
 * 这就是 Windows 上「在 execve 之前把自己降权」的那一步。
 * 与 Landlock 的差别只在时机：Linux 是子进程自己 restrict_self，
 * Windows 是父进程在建进程时就把 token 定死，子进程没有任何窗口期。
 */
bool FillSecurityCapabilities(const Confinement& c, SECURITY_CAPABILITIES* out);

}  // namespace hx

#endif  // _WIN32
