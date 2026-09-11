// hxd —— hx 引擎。JSONL over stdio，见 proto/hxp-v0.md
//
// 这个进程是整个系统里唯一被允许碰真实世界的地方：起进程、读写文件、装沙箱。
// TS 宿主没有 fs / child_process，一切副作用都要从这里过。
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "engine.hpp"
#include "exec/env.hpp"
#include "exec/rlimit.hpp"
#include "exec/spawn.hpp"
#include "proto.hpp"
#include "sandbox/caps.hpp"
#include "sandbox/landlock.hpp"
#include "sandbox/policy.hpp"
#include "sandbox/seccomp.hpp"

namespace hx {
namespace {

// 输出与事件循环都在 engine.cpp；这里只留 CLI 入口与 --sandbox-exec。

bool WriteLine(const json& j) {
  std::string s = j.dump();
  s.push_back('\n');
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::write(STDOUT_FILENO, s.data() + off, s.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

int PrintSelfTest() {
  const Caps c = DetectCaps();
  WriteLine(CapsToJson(c));
  // 退出码携带最重要的那条结论：没有 Landlock 就没有真沙箱。
  return c.landlock_abi > 0 ? 0 : 2;
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
        ::fprintf(stderr, "hxd: bad --mode\n");
        return 64;
      }
    } else if (a == "--net" && i + 1 < argc) {
      if (!ParseNetMode(argv[++i], &p.net)) {
        ::fprintf(stderr, "hxd: bad --net\n");
        return 64;
      }
    } else if (a == "--root" && i + 1 < argc) {
      char* real = ::realpath(argv[++i], nullptr);
      if (real == nullptr) {
        ::fprintf(stderr, "hxd: --root does not resolve: %s\n", argv[i]);
        return 66;
      }
      p.roots.emplace_back(real);
      ::free(real);
    } else {
      ::fprintf(stderr, "hxd: unexpected arg: %s\n", argv[i]);
      return 64;
    }
  }

  if (cmd.empty()) {
    ::fprintf(stderr, "hxd: --sandbox-exec needs: [opts] -- <cmd> [args...]\n");
    return 64;
  }

  // 与 session.open 一致：建会话私有 tmp 并授权，否则编译器等工具会去写 /tmp 而失败
  char tmpl[] = "/tmp/hx-exec-XXXXXX";
  if (::mkdtemp(tmpl) != nullptr) p.tmpdir = tmpl;

  const Caps caps = DetectCaps();
  RulesetBuild build = BuildRulesetFd(p, caps);
  for (const auto& w : build.warnings) {
    ::fprintf(stderr, "hxd: warning: %s\n", w.c_str());
  }
  ::fprintf(stderr, "hxd: sandbox=%s net=%s landlock_abi=%d enforced=%s net_enforced=%s\n",
            ToString(p.sandbox), ToString(p.net), caps.landlock_abi,
            build.enforced ? "yes" : "NO", build.net_enforced ? "yes" : "NO");

  const std::string cwd = p.roots.empty() ? std::string() : p.roots.front();
  const std::vector<std::string> env = BuildMinimalEnv(cwd, p.tmpdir, {});
  SpawnResult r = RunForeground(cmd, cwd, env, build.fd, PlanFor(p), DefaultLimits());
  if (build.fd >= 0) ::close(build.fd);

  if (!r.started) {
    ::fprintf(stderr, "hxd: %s\n", r.error.c_str());
    return 70;
  }
  return r.exit_code;
}

void PrintUsage() {
  ::fprintf(stderr,
            "hxd — hx engine (hxp/0)\n"
            "\n"
            "usage:\n"
            "  hxd                 read JSONL requests on stdin, write replies/events on stdout\n"
            "  hxd --self-test     print kernel isolation capabilities as JSON and exit\n"
            "                      (exit 0 = Landlock available, 2 = no real sandbox)\n"
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
    ::fprintf(stderr, "hxd: unknown argument: %s\n", argv[i]);
    hx::PrintUsage();
    return 64;  // EX_USAGE
  }
  hx::Engine engine;
  return engine.Run();
}
