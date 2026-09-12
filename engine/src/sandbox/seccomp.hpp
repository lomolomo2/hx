// seccomp-bpf 过滤器。
//
// 存在的理由只有一个：补上 Landlock 管不到的地方。
//   · Landlock ABI 4 只管 TCP bind/connect —— UDP 完全不在它的视野里，
//     所以 net:deny 的会话仍然能往外发 UDP（DNS 就是最现成的通道）。
//   · 另有一批 syscall 与"跑一个编译任务"无关，但很适合用来越狱：
//     ptrace / mount / pivot_root / keyctl / bpf / 装载内核模块。
//
// ★ 这一层只做「减法」：不放行任何东西，只在 Landlock 之上再封死一些。
//   两层的失败模式不同，叠起来才有意义。
#pragma once

#include <string>

#include "sandbox/policy.hpp"

namespace hx {

struct SeccompPlan {
  bool block_inet = false;  // 封死 AF_INET/AF_INET6 的 socket()，覆盖 UDP
  bool block_admin = true;  // 封死 ptrace/mount/keyctl/bpf 等
};

#ifndef _WIN32
// 在 fork 之后、execve 之前调用（与 Landlock 同一位置）。
// 失败返回 false 并填 err —— 调用方必须当作致命错误，绝不降级执行。
bool ApplySeccomp(const SeccompPlan& plan, std::string* err);
#endif
// ★ Windows 上**没有**这一层，而且不是「还没做」：
//   seccomp 在 Linux 存在的理由是补 Landlock 的 UDP 盲区，
//   而 AppContainer 不给 internetClient 时 WFP 连 UDP 一起挡，盲区本身不存在。
//   硬塞一个返回 true 的空实现会让日志里的 net_enforced 变成谎话，
//   所以宁可让它在 Windows 上不存在，谁调用谁编译不过。

// 策略 -> 计划。纯粹的映射，两个平台共用（见 policy.cpp）。
SeccompPlan PlanFor(const Policy& p);

}  // namespace hx
