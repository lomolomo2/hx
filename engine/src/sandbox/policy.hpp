// 沙箱策略。注意两件事是正交的（见 harness-windows-kernel-analogy.md §2.4）：
//   SandboxMode —— 能做什么（内核强制，本文件）
//   ApprovalPolicy —— 谁来批（宿主侧策略，不在引擎里）
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
  std::vector<std::string> roots;  // 绝对路径，已 realpath
  std::string tmpdir;              // 会话私有可写 tmp；空则不授予

  // 额外的只读路径。默认系统路径故意不含 $HOME、/sys/fs/cgroup 之类，
  // 但有些任务确实需要：
  //   · 工具链装在 $HOME（nvm / rustup / conda）
  //   · 任务本身就是关于某个系统接口的（比如实现 cgroup 支持）
  // 显式授予好过放宽默认值 —— 默认保持窄，按需开口。
  std::vector<std::string> extra_read_paths;
};

bool ParseSandboxMode(std::string_view s, SandboxMode* out);
bool ParseNetMode(std::string_view s, NetMode* out);
const char* ToString(SandboxMode m);
const char* ToString(NetMode m);

// 策略只能收紧不能放松（proto/hxp-v0.md §3.3）。
// 返回 true 表示 next 不比 cur 更宽松。
bool IsNotLooser(const Policy& cur, const Policy& next);

// 系统只读路径：不含 $HOME —— 这正是 `cat ~/.ssh/id_rsa` 会 EACCES 的原因。
// Landlock 是 allow-list，没授予就是拒绝。
const std::vector<std::string>& DefaultSystemReadPaths();

// 始终可写的设备节点（/dev/null 之类，不给就几乎所有工具都跑不起来）
const std::vector<std::string>& DefaultWritableDevices();

}  // namespace hx
