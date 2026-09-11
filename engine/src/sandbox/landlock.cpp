#include "sandbox/landlock.hpp"

#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

namespace hx {
namespace {

long CreateRuleset(const struct landlock_ruleset_attr* attr, size_t size, uint32_t flags) {
  return ::syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
long AddRule(int fd, enum landlock_rule_type type, const void* attr, uint32_t flags) {
  return ::syscall(__NR_landlock_add_rule, fd, type, attr, flags);
}
long RestrictSelf(int fd, uint32_t flags) {
  return ::syscall(__NR_landlock_restrict_self, fd, flags);
}

constexpr uint64_t kReadAccess = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE |
                                LANDLOCK_ACCESS_FS_READ_DIR;

uint64_t WriteAccessForAbi(int abi) {
  uint64_t w = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
               LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
               LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
               LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
               LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
  if (abi >= 2) w |= LANDLOCK_ACCESS_FS_REFER;    // 跨目录 rename/link，apply_patch 需要
  if (abi >= 3) w |= LANDLOCK_ACCESS_FS_TRUNCATE;
  return w;
}

// 非目录只能被授予"文件类"权限。把目录专用位（MAKE_*/READ_DIR/REMOVE_*/REFER）
// 用在普通文件或设备节点上，内核直接返回 EINVAL —— /dev/null 授权失败就是这么来的。
constexpr uint64_t kFileApplicableAccess = LANDLOCK_ACCESS_FS_EXECUTE |
                                           LANDLOCK_ACCESS_FS_WRITE_FILE |
                                           LANDLOCK_ACCESS_FS_READ_FILE |
                                           LANDLOCK_ACCESS_FS_TRUNCATE;

// 给路径授权。路径不存在只记警告，不算失败——不同发行版路径差异很大。
bool GrantPath(int ruleset_fd, const std::string& path, uint64_t access,
               std::vector<std::string>* warnings) {
  const int pfd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
  if (pfd < 0) {
    if (errno != ENOENT) {
      warnings->push_back("cannot open for rule: " + path + ": " + ::strerror(errno));
    }
    return false;
  }

  struct stat st {};
  if (::fstat(pfd, &st) == 0 && !S_ISDIR(st.st_mode)) {
    access &= kFileApplicableAccess;
    if (access == 0) {
      ::close(pfd);
      return true;  // 该文件在本策略下无可授予的权限，属正常情况
    }
  }

  struct landlock_path_beneath_attr pb {};
  pb.allowed_access = access;
  pb.parent_fd = pfd;
  const long rc = AddRule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
  const int saved = errno;
  ::close(pfd);
  if (rc != 0) {
    warnings->push_back("landlock_add_rule failed for " + path + ": " + ::strerror(saved));
    return false;
  }
  return true;
}

}  // namespace

uint64_t FsAccessMaskForAbi(int abi) {
  return kReadAccess | WriteAccessForAbi(abi);
}

RulesetBuild BuildRulesetFd(const Policy& p, const Caps& caps) {
  RulesetBuild out;

  if (p.sandbox == SandboxMode::kDangerFullAccess) {
    out.warnings.push_back("sandbox disabled: danger-full-access requested");
    return out;
  }
  if (caps.landlock_abi <= 0) {
    out.warnings.push_back(
        "landlock unavailable on this kernel: NO filesystem isolation is in effect");
    return out;
  }

  const int abi = caps.landlock_abi;
  struct landlock_ruleset_attr attr {};
  attr.handled_access_fs = FsAccessMaskForAbi(abi);

  const bool want_net_deny = (p.net == NetMode::kDeny);
  if (want_net_deny && LandlockHasNet(abi)) {
    attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    out.net_enforced = true;
  } else if (want_net_deny) {
    out.warnings.push_back(
        "network deny requested but landlock ABI " + std::to_string(abi) +
        " < 4: TCP is NOT restricted");
  }

  const long fd = CreateRuleset(&attr, sizeof(attr), 0);
  if (fd < 0) {
    out.warnings.push_back(std::string("landlock_create_ruleset failed: ") + ::strerror(errno) +
                           " — NO filesystem isolation is in effect");
    return out;
  }
  out.fd = static_cast<int>(fd);

  // 只读：系统路径 + roots 都只给读
  // 可写：系统路径给读，roots / tmpdir / 设备节点给读写
  const uint64_t write_access = kReadAccess | WriteAccessForAbi(abi);

  for (const auto& path : DefaultSystemReadPaths()) {
    GrantPath(out.fd, path, kReadAccess, &out.warnings);
  }

  // 设备节点在任何模式下都要可写：写 /dev/null 是空操作，不是安全边界。
  // 少了它，read-only 会话里每条 shell 命令都会被 profile 脚本的
  // "/dev/null: Permission denied" 噪音污染 —— 白烧 token，还会误导模型。
  for (const auto& dev : DefaultWritableDevices()) {
    GrantPath(out.fd, dev, write_access, &out.warnings);
  }

  for (const auto& extra : p.extra_read_paths) {
    if (!GrantPath(out.fd, extra, kReadAccess, &out.warnings)) {
      out.warnings.push_back("extra read path not granted: " + extra);
    }
  }

  for (const auto& root : p.roots) {
    const uint64_t access =
        (p.sandbox == SandboxMode::kWorkspaceWrite) ? write_access : kReadAccess;
    if (!GrantPath(out.fd, root, access, &out.warnings)) {
      out.warnings.push_back("root not granted: " + root);
    }
  }

  if (p.sandbox == SandboxMode::kWorkspaceWrite && !p.tmpdir.empty()) {
    GrantPath(out.fd, p.tmpdir, write_access, &out.warnings);
  }

  out.enforced = true;
  return out;
}

bool ApplyRestrictSelf(int ruleset_fd, std::string* err) {
  if (ruleset_fd < 0) return true;  // 明确不施加

  // landlock_restrict_self 要求进程已设置 no_new_privs
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    *err = std::string("prctl(PR_SET_NO_NEW_PRIVS): ") + ::strerror(errno);
    return false;
  }
  if (RestrictSelf(ruleset_fd, 0) != 0) {
    *err = std::string("landlock_restrict_self: ") + ::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace hx
