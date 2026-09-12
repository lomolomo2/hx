// 约束载体（confinement）—— 沙箱层的平台 seam。
//
// 两个平台的「内核强制」形状完全不同，但**契约相同**：
//
//   Linux    Landlock ruleset fd。父进程构建，fork 之后在子进程里
//            landlock_restrict_self()，然后 execve。
//            allow-list：没授予就是拒绝。
//
//   Windows  AppContainer。父进程派生一个每会话唯一的 AppContainer SID，
//            在 roots 上打 ACE，CreateProcess 时通过
//            PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES 把 token 降进去。
//            同样是 allow-list：AppContainer 进程只能访问 DACL 里明确
//            授予了它的 SID（或 ALL APPLICATION PACKAGES）的对象。
//
// ★ 两边共有的那条不变式必须留住：**装不上就不执行**。
//   enforced == false 且策略不是 danger-full-access 时，调用方必须拒绝起进程，
//   绝不降级为无保护运行。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sandbox/caps.hpp"
#include "sandbox/policy.hpp"

namespace hx {

/**
 * 平台私有的约束对象。
 *
 * 故意只在这里前置声明：engine.cpp 不该知道它是一个 fd 还是一个 SID。
 * 定义在 confine_posix.hpp / confine_win.hpp，只有对应平台的 spawn 会包。
 */
struct Confinement;

struct RulesetBuild {
  /** 空 = 不施加（danger-full-access，或内核不支持而已在 warnings 里说明）。 */
  std::shared_ptr<Confinement> conf;
  bool enforced = false;              // 是否真的会有内核强制
  bool net_enforced = false;          // 网络限制是否真的生效
  std::vector<std::string> warnings;  // 降级说明，必须如实上报给 host
};

/** 在父进程构建。返回的对象可被多次 spawn 复用。 */
RulesetBuild BuildConfinement(const Policy& p, const Caps& caps);

}  // namespace hx
