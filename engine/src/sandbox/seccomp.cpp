#ifndef _WIN32
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

// Syscalls that have nothing to do with "running a build" but are well suited
// to breaking out. They return EPERM rather than killing the process outright:
// a tool that gets an error code can report it itself, whereas an outright
// KILL becomes an unexplained crash and the model just retries repeatedly.
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

bool ApplySeccomp(const SeccompPlan& plan, std::string* err) {
  if (!plan.block_inet && !plan.block_admin) return true;

  std::vector<sock_filter> prog;
  const auto emit = [&prog](sock_filter f) { prog.push_back(f); };

  // Kill outright on an architecture mismatch: otherwise the 32-bit
  // compatibility entry point can bypass a filter hardcoded by syscall number
  emit(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
  emit(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kExpectedArch, 1, 0));
  emit(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));

  emit(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));

  // Record the index of every instruction that needs to jump to DENY, and
  // backpatch all the offsets at the end
  std::vector<size_t> to_deny;

  if (plan.block_admin) {
    for (const long nr : kAdminSyscalls) {
      to_deny.push_back(prog.size());
      emit(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 0));
    }
  }

  if (plan.block_inet) {
    // if (nr == socket) { load args[0]; if (domain == AF_INET || AF_INET6) deny; }
    // Not socket: skip the following 3 instructions (the load and two comparisons)
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

  // Backpatch: jt points at DENY, jf falls through to the next instruction
  for (const size_t at : to_deny) {
    const size_t delta = deny_idx - at - 1;
    if (delta > 255) {  // BPF jump offsets are only 8 bits
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

  // seccomp requires the process to have given up privilege escalation
  // (Landlock sets this too; setting it twice is idempotent)
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    *err = std::string("prctl(PR_SET_NO_NEW_PRIVS): ") + ::strerror(errno);
    return false;
  }
  if (::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0u, &fprog) != 0) {
    // Older kernels lack seccomp(2); fall back to the prctl interface
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) != 0) {
      *err = std::string("seccomp(SET_MODE_FILTER): ") + ::strerror(errno);
      return false;
    }
  }
  return true;
}

}  // namespace hx
#endif  // !_WIN32
