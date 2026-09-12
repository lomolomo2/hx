#include "sandbox/landlock.hpp"

#include <memory>

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
  if (abi >= 2) w |= LANDLOCK_ACCESS_FS_REFER;    // cross-directory rename/link, which apply_patch needs
  if (abi >= 3) w |= LANDLOCK_ACCESS_FS_TRUNCATE;
  return w;
}

// Non-directories can only be granted "file-class" permissions. Use a
// directory-only bit (MAKE_*/READ_DIR/REMOVE_*/REFER) on a regular file or a
// device node and the kernel returns EINVAL outright -- which is exactly how
// granting /dev/null came to fail.
constexpr uint64_t kFileApplicableAccess = LANDLOCK_ACCESS_FS_EXECUTE |
                                           LANDLOCK_ACCESS_FS_WRITE_FILE |
                                           LANDLOCK_ACCESS_FS_READ_FILE |
                                           LANDLOCK_ACCESS_FS_TRUNCATE;

// Grant access to a path. A non-existent path is only a warning, not a
// failure -- paths vary a great deal between distributions.
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
      return true;  // no grantable permission for this file under this policy; normal
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

RulesetBuild BuildConfinement(const Policy& p, const Caps& caps) {
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
  out.conf = std::make_shared<Confinement>();
  out.conf->fd = static_cast<int>(fd);

  // read-only: system paths and roots alike get read only
  // writable:  system paths get read; roots / tmpdir / device nodes get read+write
  const uint64_t write_access = kReadAccess | WriteAccessForAbi(abi);

  for (const auto& path : DefaultSystemReadPaths()) {
    GrantPath(out.conf->fd, path, kReadAccess, &out.warnings);
  }

  // Device nodes must be writable in every mode: writing to /dev/null is a
  // no-op, not a security boundary. Without it, every shell command in a
  // read-only session gets polluted by "/dev/null: Permission denied" noise
  // from profile scripts -- burning tokens for nothing and misleading the
  // model besides.
  for (const auto& dev : DefaultWritableDevices()) {
    GrantPath(out.conf->fd, dev, write_access, &out.warnings);
  }

  for (const auto& extra : p.extra_read_paths) {
    if (!GrantPath(out.conf->fd, extra, kReadAccess, &out.warnings)) {
      out.warnings.push_back("extra read path not granted: " + extra);
    }
  }

  for (const auto& root : p.roots) {
    const uint64_t access =
        (p.sandbox == SandboxMode::kWorkspaceWrite) ? write_access : kReadAccess;
    if (!GrantPath(out.conf->fd, root, access, &out.warnings)) {
      out.warnings.push_back("root not granted: " + root);
    }
  }

  if (p.sandbox == SandboxMode::kWorkspaceWrite && !p.tmpdir.empty()) {
    GrantPath(out.conf->fd, p.tmpdir, write_access, &out.warnings);
  }

  out.enforced = true;
  return out;
}

bool ApplyRestrictSelf(const Confinement* conf, std::string* err) {
  if (conf == nullptr || conf->fd < 0) return true;  // explicitly not applied
  const int ruleset_fd = conf->fd;

  // landlock_restrict_self requires the process to have set no_new_privs
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
