// hxd -- the hx engine. JSONL over stdio; see proto/hxp-v0.md
//
// This process is the only place in the whole system allowed to touch the real
// world: starting processes, reading and writing files, applying the sandbox.
// The TypeScript host has no fs / child_process, so every side effect passes
// through here.
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

// Output and the event loop both live in engine.cpp; all that remains here is
// the CLI entry point and --sandbox-exec.
//
// ★ Both of those paths run before io::InitStdio, so they use stdio rather
//   than io:: -- they are one-shot synchronous output and need none of the
//   event loop's non-blocking machinery.
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
  // The exit code carries the single most important conclusion: whether this
  // machine actually has a sandbox.
  return HasRealSandbox(c) ? 0 : 2;
}

// --sandbox-exec: run one command in the foreground under a given policy.
// It exists so the sandbox kernel can be verified with real commands before
// any protocol is involved (the escape test suite uses it).
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

  // ★ Take the same path as session.open: create the session-private tmp and
  //   grant it. One of the README's lessons is "the test path must equal the
  //   production path" -- leave TMPDIR unset in --sandbox-exec and the escape
  //   tests pass in an environment that does not exist in a real session at
  //   all. This trap has been hit three times in this project.
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
