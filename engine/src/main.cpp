// hxd —— hx 引擎。JSONL over stdio，见 proto/hxp-v0.md
//
// 这个进程是整个系统里唯一被允许碰真实世界的地方：起进程、读写文件、装沙箱。
// TS 宿主没有 fs / child_process，一切副作用都要从这里过。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "engine.hpp"
#include "exec/env.hpp"
#include "exec/rlimit.hpp"
#include "exec/spawn.hpp"
#include "platform/platform.hpp"
#include "proto.hpp"
#include "sandbox/caps.hpp"
#include "sandbox/confine.hpp"
#include "sandbox/policy.hpp"
#include "sandbox/seccomp.hpp"

namespace hx {
namespace {

// 输出与事件循环都在 engine.cpp；这里只留 CLI 入口与 --sandbox-exec。
//
// ★ 这两条路径都在 io::InitStdio 之前跑，所以用 stdio 而不是 io:: ——
//   它们是一次性的同步输出，不需要事件循环那套非阻塞机制。
bool WriteLine(const json& j) {
  std::string s = j.dump();
  s.push_back('\n');
  const size_t n = std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
  return n == s.size();
}

int PrintSelfTest() {
  const Caps c = DetectCaps();
  WriteLine(CapsToJson(c));
  // 退出码携带最重要的那条结论：这台机器上到底有没有真沙箱。
  return HasRealSandbox(c) ? 0 : 2;
}

// --sandbox-exec：在给定策略下前台跑一条命令。
// 存在的意义是能在接协议之前，用真实命令验证沙箱内核（逃逸测试套件用它）。
int SandboxExec(int argc, char** argv, int start) {
  Policy p;
  std::vector<std::string> cmd;
  bool saw_sep = false;

  for (int i = start; i < argc; ++i) {
    const std::string_view a(argv[i]);
    if (saw_sep) {
      cmd.emplace_back(argv[i]);
      continue;
    }
    if (a == "--") {
      saw_sep = true;
    } else if (a == "--mode" && i + 1 < argc) {
      if (!ParseSandboxMode(argv[++i], &p.sandbox)) {
        std::fprintf(stderr, "hxd: bad --mode\n");
        return 64;
      }
    } else if (a == "--net" && i + 1 < argc) {
      if (!ParseNetMode(argv[++i], &p.net)) {
        std::fprintf(stderr, "hxd: bad --net\n");
        return 64;
      }
    } else if (a == "--root" && i + 1 < argc) {
      std::string real;
      if (!platform::RealPath(argv[++i], &real)) {
        std::fprintf(stderr, "hxd: --root does not resolve: %s\n", argv[i]);
        return 66;
      }
      p.roots.push_back(std::move(real));
    } else {
      std::fprintf(stderr, "hxd: unexpected arg: %s\n", argv[i]);
      return 64;
    }
  }

  if (cmd.empty()) {
    std::fprintf(stderr, "hxd: --sandbox-exec needs: [opts] -- <cmd> [args...]\n");
    return 64;
  }

  // ★ 与 session.open 走同一条路：建会话私有 tmp 并授权。
  //   README 的教训之一就是「测试路径必须等于生产路径」——
  //   --sandbox-exec 少设一个 TMPDIR，就会让逃逸测试在一个真实会话里
  //   根本不存在的环境下通过。这个坑在本项目里踩过三次。
  std::string tmperr;
  if (!platform::MakeTempDir("hx-exec-", &p.tmpdir, &tmperr)) {
    std::fprintf(stderr, "hxd: warning: no session tmpdir: %s\n", tmperr.c_str());
  }

  const Caps caps = DetectCaps();
  RulesetBuild build = BuildConfinement(p, caps);
  for (const auto& w : build.warnings) {
    std::fprintf(stderr, "hxd: warning: %s\n", w.c_str());
  }
  std::fprintf(stderr, "hxd: sandbox=%s net=%s backend=%s enforced=%s net_enforced=%s\n",
               ToString(p.sandbox), ToString(p.net),
               HasRealSandbox(caps) ? (caps.landlock_abi > 0 ? "landlock" : "appcontainer")
                                    : "none",
               build.enforced ? "yes" : "NO", build.net_enforced ? "yes" : "NO");

  const std::string cwd = p.roots.empty() ? std::string() : p.roots.front();
  const std::vector<std::string> env = BuildMinimalEnv(cwd, p.tmpdir, {});
  SpawnResult r =
      RunForeground(cmd, cwd, env, build.conf.get(), PlanFor(p), DefaultLimits());

  if (!r.started) {
    std::fprintf(stderr, "hxd: %s\n", r.error.c_str());
    return 70;
  }
  return r.exit_code;
}

void PrintUsage() {
  std::fprintf(stderr,
               "hxd - hx engine (hxp/0)\n"
               "\n"
               "usage:\n"
               "  hxd                 read JSONL requests on stdin, write replies/events on stdout\n"
               "  hxd --self-test     print kernel isolation capabilities as JSON and exit\n"
               "                      (exit 0 = real sandbox available, 2 = none)\n"
               "  hxd --sandbox-exec [--mode M] [--net N] [--root P]... -- CMD [ARGS...]\n"
               "                      run CMD under the given sandbox policy (for escape tests)\n"
               "  hxd --help\n"
               "\n"
               "protocol: proto/hxp-v0.md\n");
}

}  // namespace
}  // namespace hx

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--self-test") {
      return hx::PrintSelfTest();
    }
    if (arg == "--sandbox-exec") {
      return hx::SandboxExec(argc, argv, i + 1);
    }
    if (arg == "--help" || arg == "-h") {
      hx::PrintUsage();
      return 0;
    }
    std::fprintf(stderr, "hxd: unknown argument: %s\n", argv[i]);
    hx::PrintUsage();
    return 64;  // EX_USAGE
  }
  hx::Engine engine;
  return engine.Run();
}
