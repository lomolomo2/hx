// Sandbox policy. Note that two things here are orthogonal (see
// harness-windows-kernel-analogy.md section 2.4):
//   SandboxMode     -- what can be done (kernel-enforced; this file)
//   ApprovalPolicy  -- who approves it (host-side policy, not in the engine)
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hx {

enum class SandboxMode {
  kReadOnly = 0,
  kWorkspaceWrite = 1,
  kDangerFullAccess = 2,
};

enum class NetMode {
  kDeny = 0,
  kAllow = 1,
};

struct Policy {
  SandboxMode sandbox = SandboxMode::kReadOnly;
  NetMode net = NetMode::kDeny;
  std::vector<std::string> roots;  // absolute, already through realpath
  std::string tmpdir;              // session-private writable tmp; empty means not granted

  // Extra read-only paths. The default system paths deliberately exclude
  // things like $HOME and /sys/fs/cgroup, but some tasks genuinely need them:
  //   - a toolchain installed in $HOME (nvm / rustup / conda)
  //   - a task that is itself about some system interface (implementing
  //     cgroup support, say)
  // An explicit grant beats widening the defaults -- keep the default narrow
  // and open holes on demand.
  std::vector<std::string> extra_read_paths;
};

bool ParseSandboxMode(std::string_view s, SandboxMode* out);
bool ParseNetMode(std::string_view s, NetMode* out);
const char* ToString(SandboxMode m);
const char* ToString(NetMode m);

// A policy can only tighten, never loosen (proto/hxp-v0.md section 3.3).
// Returns true when next is no looser than cur.
bool IsNotLooser(const Policy& cur, const Policy& next);

// System read-only paths: $HOME is not among them -- which is precisely why
// `cat ~/.ssh/id_rsa` gets EACCES. Landlock is an allow-list, so not granted
// means denied.
const std::vector<std::string>& DefaultSystemReadPaths();

// Device nodes that are always writable (/dev/null and the like; without them
// almost no tool runs at all)
const std::vector<std::string>& DefaultWritableDevices();

}  // namespace hx
