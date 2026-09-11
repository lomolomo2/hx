#include "sandbox/seccomp.hpp"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif

namespace hx {
namespace {

#if defined(__x86_64__)
constexpr uint32_t kExpectedArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr uint32_t kExpectedArch = AUDIT_ARCH_AARCH64;
#else
#error "hx seccomp: unsupported architecture"
#endif

// 与"跑一个构建任务"无关，但很适合用来越狱的 syscall。
// 返回 EPERM 而不是直接杀进程：工具拿到一个错误码能自己报告，
// 直接 KILL 会变成一个没有解释的崩溃，模型只会反复重试。
const long kAdminSyscalls[] = {
    __NR_ptrace,       __NR_mount,        __NR_umount2,      __NR_pivot_root,
    __NR_chroot,       __NR_keyctl,       __NR_add_key,      __NR_request_key,
    __NR_init_module,  __NR_finit_module, __NR_delete_module, __NR_kexec_load,
    __NR_reboot,       __NR_swapon,       __NR_swapoff,      __NR_setns,
    __NR_perf_event_open, __NR_process_vm_readv, __NR_process_vm_writev,
#ifdef __NR_bpf
    __NR_bpf,
#endif
#ifdef __NR_kexec_file_load
    __NR_kexec_file_load,
#endif
};

constexpr uint32_t kDeny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);

}  // namespace

SeccompPlan PlanFor(const Policy& p) {
  SeccompPlan plan;
  // Landlock 只管 TCP。要真的断网，必须在这里把 AF_INET/AF_INET6 的
  // socket() 一起封掉 —— 否则 UDP（含 DNS）畅通无阻。
  plan.block_inet = (p.net == NetMode::kDeny);
  plan.block_admin = (p.sandbox != SandboxMode::kDangerFullAccess);
  return plan;
}

bool ApplySeccomp(const SeccompPlan& plan, std::string* err) {
  if (!plan.block_inet && !plan.block_admin) return true;

  std::vector<sock_filter> prog;
  const auto emit = [&prog](sock_filter f) { prog.push_back(f); };

  // 架构不符直接杀：否则 32 位兼容入口可以绕开按编号写死的过滤器
  emit(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
  emit(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kExpectedArch, 1, 0));
  emit(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));

  emit(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));

  // 记录所有需要跳到 DENY 的指令下标，最后统一回填偏移
  std::vector<size_t> to_deny;

  if (plan.block_admin) {
    for (const long nr : kAdminSyscalls) {
      to_deny.push_back(prog.size());
      emit(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 0));
    }
  }

  if (plan.block_inet) {
    // if (nr == socket) { load args[0]; if (domain == AF_INET || AF_INET6) deny; }
    // 不是 socket 就跳过后面 3 条（load + 两次比较）
    emit(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(__NR_socket), 0, 3));
    emit(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
    to_deny.push_back(prog.size());
    emit(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 0, 0));
    to_deny.push_back(prog.size());
    emit(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 0, 0));
  }

  const size_t allow_idx = prog.size();
  emit(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  const size_t deny_idx = prog.size();
  emit(BPF_STMT(BPF_RET | BPF_K, kDeny));

  // 回填：jt 指向 DENY，jf 落到下一条
  for (const size_t at : to_deny) {
    const size_t delta = deny_idx - at - 1;
    if (delta > 255) {  // BPF 的跳转偏移只有 8 位
      *err = "seccomp filter too long for 8-bit jumps";
      return false;
    }
    prog[at].jt = static_cast<uint8_t>(delta);
    prog[at].jf = 0;
  }
  (void)allow_idx;

  sock_fprog fprog{};
  fprog.len = static_cast<unsigned short>(prog.size());
  fprog.filter = prog.data();

  // seccomp 要求进程已放弃提权能力（Landlock 那边也会设，重复设是幂等的）
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    *err = std::string("prctl(PR_SET_NO_NEW_PRIVS): ") + ::strerror(errno);
    return false;
  }
  if (::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0u, &fprog) != 0) {
    // 老内核没有 seccomp(2)，退回 prctl 接口
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) != 0) {
      *err = std::string("seccomp(SET_MODE_FILTER): ") + ::strerror(errno);
      return false;
    }
  }
  return true;
}

}  // namespace hx
