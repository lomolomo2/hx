#include "sandbox/policy.hpp"

namespace hx {

bool ParseSandboxMode(std::string_view s, SandboxMode* out) {
  if (s == "read-only") { *out = SandboxMode::kReadOnly; return true; }
  if (s == "workspace-write") { *out = SandboxMode::kWorkspaceWrite; return true; }
  if (s == "danger-full-access") { *out = SandboxMode::kDangerFullAccess; return true; }
  return false;
}

bool ParseNetMode(std::string_view s, NetMode* out) {
  if (s == "deny") { *out = NetMode::kDeny; return true; }
  if (s == "allow") { *out = NetMode::kAllow; return true; }
  return false;
}

const char* ToString(SandboxMode m) {
  switch (m) {
    case SandboxMode::kReadOnly: return "read-only";
    case SandboxMode::kWorkspaceWrite: return "workspace-write";
    case SandboxMode::kDangerFullAccess: return "danger-full-access";
  }
  return "unknown";
}

const char* ToString(NetMode m) {
  switch (m) {
    case NetMode::kDeny: return "deny";
    case NetMode::kAllow: return "allow";
  }
  return "unknown";
}

bool IsNotLooser(const Policy& cur, const Policy& next) {
  if (static_cast<int>(next.sandbox) > static_cast<int>(cur.sandbox)) return false;
  if (static_cast<int>(next.net) > static_cast<int>(cur.net)) return false;
  // 额外只读路径同样只能收窄
  for (const auto& e : next.extra_read_paths) {
    bool found = false;
    for (const auto& c : cur.extra_read_paths) {
      if (e == c) { found = true; break; }
    }
    if (!found) return false;
  }
  // roots 只能是当前集合的子集
  for (const auto& r : next.roots) {
    bool found = false;
    for (const auto& c : cur.roots) {
      if (r == c) { found = true; break; }
    }
    if (!found) return false;
  }
  return true;
}

const std::vector<std::string>& DefaultSystemReadPaths() {
  // 故意不含 /home、/root、/var/lib、/mnt：agent 要读的东西必须显式进 roots。
  static const std::vector<std::string> kPaths = {
      "/usr", "/bin", "/sbin", "/lib", "/lib64", "/opt",
      "/etc",            // resolv.conf / ssl 证书 / 时区
      "/proc",           // 大量工具依赖
      "/sys/devices",    // nproc 之类
      "/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom",
      "/dev/tty", "/dev/pts", "/dev/ptmx",
  };
  return kPaths;
}

const std::vector<std::string>& DefaultWritableDevices() {
  static const std::vector<std::string> kPaths = {
      "/dev/null", "/dev/zero", "/dev/full", "/dev/tty", "/dev/pts", "/dev/ptmx",
  };
  return kPaths;
}

}  // namespace hx
