#include "sandbox/policy.hpp"

#include "sandbox/seccomp.hpp"

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
  // Extra read-only paths can likewise only narrow
  for (const auto& e : next.extra_read_paths) {
    bool found = false;
    for (const auto& c : cur.extra_read_paths) {
      if (e == c) { found = true; break; }
    }
    if (!found) return false;
  }
  // roots can only be a subset of the current set
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
  // /home, /root, /var/lib and /mnt are deliberately absent: anything the
  // agent needs to read must enter roots explicitly.
  static const std::vector<std::string> kPaths = {
      "/usr", "/bin", "/sbin", "/lib", "/lib64", "/opt",
      "/etc",            // resolv.conf / ssl certificates / timezone
      "/proc",           // a great many tools depend on it
      "/sys/devices",    // nproc and the like
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

SeccompPlan PlanFor(const Policy& p) {
  SeccompPlan plan;
  // Landlock only covers TCP. To really be offline, the Linux side has to seal
  // off socket() for AF_INET/AF_INET6 here as well -- otherwise UDP (DNS
  // included) passes freely.
  // The Windows side ignores this field: without internetClient granted to the
  // AppContainer, WFP blocks everything.
  plan.block_inet = (p.net == NetMode::kDeny);
  plan.block_admin = (p.sandbox != SandboxMode::kDangerFullAccess);
  return plan;
}

}  // namespace hx
