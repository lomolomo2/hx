#include "sandbox/caps.hpp"

#include "sandbox/cgroup.hpp"

#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

namespace hx {
namespace {

// 传 NULL + LANDLOCK_CREATE_RULESET_VERSION 是内核规定的 ABI 探测方式：
// 不创建 ruleset，只返回版本号。
int ProbeLandlockAbi() {
  const long rc = ::syscall(__NR_landlock_create_ruleset, nullptr, size_t{0},
                            LANDLOCK_CREATE_RULESET_VERSION);
  if (rc < 0) {
    return 0;  // ENOSYS（内核没编）/ EOPNOTSUPP（没在 LSM 链里启用）
  }
  return static_cast<int>(rc);
}

bool ProbeSeccomp() {
  // PR_GET_SECCOMP 在支持 seccomp 的内核上返回当前模式（0 = disabled）。
  errno = 0;
  const int rc = ::prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
  return rc >= 0;
}

bool ProbeUserns() {
  std::ifstream f("/proc/sys/user/max_user_namespaces");
  long long max_ns = 0;
  if (!(f >> max_ns)) {
    return false;
  }
  return max_ns > 0;
}

bool ProbeCgroup2(std::string* path) {
  struct statfs sfs {};
  if (::statfs("/sys/fs/cgroup", &sfs) != 0) {
    return false;
  }
  if (static_cast<unsigned long>(sfs.f_type) != CGROUP2_SUPER_MAGIC) {
    return false;
  }
  *path = "/sys/fs/cgroup";
  return true;
}

std::string ProbeKernel() {
  struct utsname u {};
  if (::uname(&u) != 0) {
    return "unknown";
  }
  return u.release;
}

}  // namespace

Caps DetectCaps() {
  Caps c;
  c.kernel = ProbeKernel();
  c.landlock_abi = ProbeLandlockAbi();
  c.seccomp = ProbeSeccomp();
  c.userns = ProbeUserns();
  c.cgroup2 = ProbeCgroup2(&c.cgroup_path);
  // 文件系统在不等于限额装得上：再走一遍"建子 cgroup + 写限额"，
  // 被无内部进程规则挡住时如实报 false 并给出原因。
  if (c.cgroup2) {
    std::string probe_path;
    c.cgroup2_usable = Cgroup2Usable(&probe_path, &c.cgroup2_reason);
  }
  return c;
}

json CapsToJson(const Caps& c) {
  return json{
      {"proto", "hxp/0"},
      {"kernel", c.kernel},
      {"landlock",
       json{{"available", c.landlock_abi > 0},
            {"abi", c.landlock_abi},
            {"fs", LandlockHasFs(c.landlock_abi)},
            {"truncate", LandlockHasTruncate(c.landlock_abi)},
            {"net", LandlockHasNet(c.landlock_abi)}}},
      {"seccomp", json{{"available", c.seccomp}}},
      {"userns", json{{"available", c.userns}}},
      {"cgroup2", json{{"available", c.cgroup2},
                       {"usable", c.cgroup2_usable},
                       {"reason", c.cgroup2_reason},
                       {"path", c.cgroup_path}}},
  };
}

}  // namespace hx
