#include "engine.hpp"
#include "line_reader.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "exec/env.hpp"
#include "exec/proc.hpp"
#include "io/io.hpp"
#include "platform/platform.hpp"
#include "exec/pty.hpp"
#include "exec/rlimit.hpp"
#include "exec/spawn.hpp"
#include "fs/apply_patch.hpp"
#include "log/rollout.hpp"
#include "fs/path_guard.hpp"

namespace hx {
namespace {

// 通配匹配：pat 中的 "**" 匹配零个或多个路径分量。
bool MatchComponents(const std::vector<std::string>& pat, size_t pi,
                     const std::vector<std::string>& path, size_t si) {
  while (pi < pat.size()) {
    if (pat[pi] == "**") {
      if (pi + 1 == pat.size()) return true;  // 结尾的 ** 吃掉剩余全部
      for (size_t k = si; k <= path.size(); ++k) {
        if (MatchComponents(pat, pi + 1, path, k)) return true;
      }
      return false;
    }
    if (si >= path.size()) return false;
    if (!platform::MatchComponent(pat[pi], path[si])) return false;
    ++pi;
    ++si;
  }
  return si == path.size();
}

std::vector<std::string> SplitComponents(const std::string& s) {
  // ★ 两种分隔符都要认。
  //   宿主写的 glob 一律是 '/'（协议是跨平台的），而引擎在 Windows 上
  //   算出来的相对路径带的是 '\'。只按 '/' 切的话，Windows 上
  //   "src/**/*.ts" 永远匹配不到任何东西 —— 而且是静默的零结果，
  //   不会报错，最难查的那种。
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || platform::IsSeparator(s[i])) {
      if (i > start) out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

}  // namespace

Engine::Engine() : caps_(DetectCaps()) { RegisterOps(); }

Engine::~Engine() {
  // ruleset_ 与 reactor_ 自己管好自己的资源（前者是 shared_ptr，
  // 后者有析构）。这里只剩会话私有 tmp —— 非空时删不掉，那是故意的：
  // 留下痕迹比悄悄递归删除一个可能装着用户数据的目录安全得多。
  if (!tmpdir_.empty()) platform::RemoveDir(tmpdir_);
}

// 出站队列上限。超过就开始丢事件 —— 丢事件比冻结引擎好得多。
static constexpr size_t kOutSoftCap = 8u * 1024u * 1024u;

void Engine::Send(std::string line, bool droppable) {
  if (droppable && out_buf_.size() + line.size() > kOutSoftCap) {
    ++dropped_events_;
    return;
  }
  out_buf_ += line;
  FlushOut();
}

void Engine::FlushOut() {
  while (!out_buf_.empty()) {
    const long n = io::Write(io::kStdout, out_buf_.data(), out_buf_.size());
    if (n > 0) {
      out_buf_.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (n == io::kWouldBlock) {
      // 写不动了：挂上「关注可写」，等可写再继续，绝不在这里等
      if (!out_watched_ && reactor_.WatchWrite(io::kStdout, true)) out_watched_ = true;
      return;
    }
    // stdout 断了：宿主没了，丢弃积压，循环会在 stdin EOF 时退出
    out_buf_.clear();
    break;
  }
  if (out_buf_.empty() && out_watched_) {
    reactor_.WatchWrite(io::kStdout, false);
    out_watched_ = false;
  }
}

void Engine::Emit(const json& j) {
  std::string line = j.dump();
  line.push_back('\n');
  Send(std::move(line), /*droppable=*/true);
}

void Engine::Reply(const json& j) {
  std::string line = j.dump();
  line.push_back('\n');
  Send(std::move(line), /*droppable=*/false);
}

// ------------------------------------------------------------------ ops

void Engine::RegisterOps() {
  ops_["ping"] = [this](const Request& r) { return OpPing(r); };
  ops_["selftest"] = [this](const Request& r) { return OpSelfTest(r); };
  ops_["session.open"] = [this](const Request& r) { return OpSessionOpen(r); };
  ops_["policy.set"] = [this](const Request& r) { return OpPolicySet(r); };
  ops_["exec.start"] = [this](const Request& r) { return OpExecStart(r); };
  ops_["exec.stdin"] = [this](const Request& r) { return OpExecStdin(r); };
  ops_["exec.wait"] = [this](const Request& r) { return OpExecWait(r); };
  ops_["exec.kill"] = [this](const Request& r) { return OpExecKill(r); };
  ops_["fs.read"] = [this](const Request& r) { return OpFsRead(r); };
  ops_["fs.apply_patch"] = [this](const Request& r) { return OpFsApplyPatch(r); };
  ops_["fs.stat"] = [this](const Request& r) { return OpFsStat(r); };
  ops_["fs.glob"] = [this](const Request& r) { return OpFsGlob(r); };
  ops_["log.append"] = [this](const Request& r) { return OpLogAppend(r); };
  ops_["log.flush"] = [this](const Request& r) { return OpLogFlush(r); };
}

std::optional<json> Engine::OpPing(const Request& r) {
  return MakeOk(r.id, json{{"pong", true}, {"pid", platform::Pid()}});
}

std::optional<json> Engine::OpSelfTest(const Request& r) {
  return MakeOk(r.id, CapsToJson(caps_));
}

json Engine::EffectiveJson() const {
  return json{{"sandbox", ToString(policy_.sandbox)},
              {"net", ToString(policy_.net)},
              {"roots", policy_.roots},
              {"extra_read_paths", policy_.extra_read_paths},
              {"tmpdir", tmpdir_},
              {"enforced", ruleset_.enforced},
              {"net_enforced", ruleset_.net_enforced},
              // ★ 不再露出 landlock_abi 这种平台专有的数字。
              //   宿主要判断的是「这次有没有真沙箱」，不是「用的哪套内核接口」；
              //   后者在 caps 里有完整记录（session_meta 第一条就写了）。
              {"backend", caps_.landlock_abi > 0 ? "landlock"
                          : caps_.appcontainer  ? "appcontainer"
                                                : "none"}};
}

std::optional<json> Engine::OpSessionOpen(const Request& r) {
  if (session_open_) {
    return MakeError(r.id, err::kDenied, "session already open; policy can only be tightened");
  }

  const auto roots_it = r.args.find("roots");
  if (roots_it == r.args.end() || !roots_it->is_array() || roots_it->empty()) {
    return MakeError(r.id, err::kBadArgs, "roots must be a non-empty array of absolute paths");
  }

  Policy p;
  for (const auto& rv : *roots_it) {
    if (!rv.is_string()) {
      return MakeError(r.id, err::kBadArgs, "roots entries must be strings");
    }
    const std::string raw = rv.get<std::string>();
    std::string resolved;
    if (!platform::RealPath(raw, &resolved)) {
      return MakeError(r.id, err::kBadArgs, "root does not resolve: " + raw);
    }
    if (!platform::IsDirectory(resolved)) {
      return MakeError(r.id, err::kBadArgs, "root is not a directory: " + resolved);
    }
    p.roots.push_back(std::move(resolved));
  }

  if (auto it = r.args.find("sandbox"); it != r.args.end()) {
    if (!it->is_string() || !ParseSandboxMode(it->get<std::string>(), &p.sandbox)) {
      return MakeError(r.id, err::kBadArgs, "invalid sandbox mode");
    }
  }
  if (auto it = r.args.find("net"); it != r.args.end()) {
    if (!it->is_string() || !ParseNetMode(it->get<std::string>(), &p.net)) {
      return MakeError(r.id, err::kBadArgs, "invalid net mode");
    }
  }

  if (auto it = r.args.find("extra_read_paths"); it != r.args.end()) {
    if (!it->is_array()) {
      return MakeError(r.id, err::kBadArgs, "extra_read_paths must be an array");
    }
    for (const auto& v : *it) {
      if (!v.is_string()) {
        return MakeError(r.id, err::kBadArgs, "extra_read_paths entries must be strings");
      }
      const std::string raw = v.get<std::string>();
      std::string resolved;
      if (!platform::RealPath(raw, &resolved)) {
        // 不存在就跳过并如实记录，不因此让整个会话开不起来
        p.extra_read_paths.push_back(raw);
        continue;
      }
      p.extra_read_paths.push_back(std::move(resolved));
    }
  }

  // 会话私有 tmp：放在 workspace 外，避免污染仓库，但显式授予写权限
  std::string tmperr;
  if (!platform::MakeTempDir("hx-sess-", &p.tmpdir, &tmperr)) {
    return MakeError(r.id, err::kInternal, tmperr);
  }

  // ★ 旧的 ruleset 由 shared_ptr 自己回收 —— 但回收的时机有语义：
  //   Windows 上 Confinement 析构会撤掉打在 roots 上的 ACE 并删掉
  //   AppContainer profile。所以这里必须先赋值再让旧的析构，
  //   顺序反了会把新会话刚打好的 ACE 一起撤掉（同一个 root，不同 SID，
  //   但 profile 删除是按名字的）。
  ruleset_ = BuildConfinement(p, caps_);
  policy_ = std::move(p);
  tmpdir_ = policy_.tmpdir;
  session_open_ = true;

  // rollout：路径由引擎决定，宿主只能给受校验的名字
  std::string name = "s" + std::to_string(platform::Pid());
  if (auto it = r.args.find("name"); it != r.args.end() && it->is_string()) {
    name = it->get<std::string>();
    if (!Rollout::IsValidName(name)) {
      return MakeError(r.id, err::kBadArgs, "name must match [A-Za-z0-9_-]{1,64}");
    }
  }
  std::string rerr;
  if (!rollout_.Open(/*base=*/"", name, &rerr)) {
    return MakeError(r.id, err::kInternal, "rollout: " + rerr);
  }
  session_id_ = name;
  // 第一条记录就把"这次到底有没有真沙箱"钉死在日志里
  rollout_.Append(json{{"type", "session_meta"},
                       {"payload", json{{"session", session_id_},
                                        {"caps", CapsToJson(caps_)},
                                        {"effective", EffectiveJson()},
                                        {"warnings", ruleset_.warnings}}}});

  json result{{"session", session_id_},
              {"rollout", rollout_.path()},
              {"effective", EffectiveJson()}};
  if (!ruleset_.warnings.empty()) {
    result["warnings"] = ruleset_.warnings;
    for (const auto& w : ruleset_.warnings) {
      Emit(MakeEvent("engine.warning", json{{"message", w}}));
    }
  }
  return MakeOk(r.id, std::move(result));
}

std::optional<json> Engine::OpPolicySet(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;

  Policy next = policy_;
  if (auto it = r.args.find("sandbox"); it != r.args.end()) {
    if (!it->is_string() || !ParseSandboxMode(it->get<std::string>(), &next.sandbox)) {
      return MakeError(r.id, err::kBadArgs, "invalid sandbox mode");
    }
  }
  if (auto it = r.args.find("net"); it != r.args.end()) {
    if (!it->is_string() || !ParseNetMode(it->get<std::string>(), &next.net)) {
      return MakeError(r.id, err::kBadArgs, "invalid net mode");
    }
  }

  if (!IsNotLooser(policy_, next)) {
    return MakeError(r.id, err::kDenied,
                     "policy can only be tightened; open a new session to loosen");
  }

  ruleset_ = BuildConfinement(next, caps_);
  policy_ = std::move(next);

  json result{{"effective", EffectiveJson()}};
  // 诚实说明：已经在跑的 cell 带的是旧 ruleset，内核层无法追溯收紧
  if (!cells_.all().empty()) {
    result["note"] = "already-running cells keep the ruleset they were started with";
  }
  return MakeOk(r.id, std::move(result));
}

bool Engine::RequireSession(const Request& r, json* err_out) const {
  if (session_open_) return true;
  *err_out = MakeError(r.id, err::kNoSession, "session.open must be called first");
  return false;
}

std::vector<std::string> Engine::BuildEnv(const json& overrides) const {
  std::map<std::string, std::string> extra;
  if (overrides.is_object()) {
    for (const auto& [k, v] : overrides.items()) {
      if (v.is_string()) extra[k] = v.get<std::string>();
    }
  }
  return BuildMinimalEnv(policy_.roots.empty() ? std::string() : policy_.roots.front(), tmpdir_,
                         extra);
}

std::optional<json> Engine::OpExecStart(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;

  const auto cmd_it = r.args.find("cmd");
  if (cmd_it == r.args.end() || !cmd_it->is_array() || cmd_it->empty()) {
    return MakeError(r.id, err::kBadArgs, "cmd must be a non-empty array");
  }
  std::vector<std::string> argv;
  for (const auto& a : *cmd_it) {
    if (!a.is_string()) return MakeError(r.id, err::kBadArgs, "cmd entries must be strings");
    argv.push_back(a.get<std::string>());
  }

  std::string cwd_in = ".";
  if (auto it = r.args.find("cwd"); it != r.args.end()) {
    if (!it->is_string()) return MakeError(r.id, err::kBadArgs, "cwd must be a string");
    cwd_in = it->get<std::string>();
  }
  const ResolveResult cwd =
      ResolveInRoots(cwd_in, policy_.roots.front(), policy_.roots, /*must_exist=*/true);
  if (!cwd.ok) {
    Emit(MakeEvent("policy.violation",
                   json{{"op", "exec.start"}, {"path", cwd_in}, {"reason", cwd.error}}));
    return MakeError(r.id, err::kPathEscape, cwd.error);
  }

  int64_t timeout_ms = 0;
  if (auto it = r.args.find("timeout_ms"); it != r.args.end()) {
    if (!it->is_number_integer() || it->get<int64_t>() < 0) {
      return MakeError(r.id, err::kBadArgs, "timeout_ms must be a non-negative integer");
    }
    timeout_ms = it->get<int64_t>();
  }

  const bool want_pty = r.args.value("pty", false);
  const json env_arg = r.args.contains("env") ? r.args.at("env") : json::object();

  Cell* c = nullptr;
  if (want_pty) {
    PtySpawnRequest pr;
    pr.argv = std::move(argv);
    pr.cwd = cwd.path;
    pr.envp = BuildEnv(env_arg);
    pr.conf = ruleset_.conf.get();
    pr.seccomp = PlanFor(policy_);
    pr.limits = DefaultLimits();
    if (auto it = r.args.find("rows"); it != r.args.end() && it->is_number_unsigned()) {
      pr.rows = static_cast<unsigned short>(it->get<unsigned>());
    }
    if (auto it = r.args.find("cols"); it != r.args.end() && it->is_number_unsigned()) {
      pr.cols = static_cast<unsigned short>(it->get<unsigned>());
    }
    PtySpawnResult sp = SpawnPty(pr);
    if (!sp.ok) return MakeError(r.id, err::kInternal, sp.error);

    c = cells_.Create(cells_.NextId());
    c->is_pty = true;
    c->proc = sp.proc;
    // ★ 别写成 in_fd = out_fd。Linux 上两者确实是同一个 fd，
    //   ConPTY 却是两条独立管道 —— 见 PtySpawnResult 的注释。
    c->out_fd = sp.master_out;
    c->in_fd = sp.master_in;
  } else {
    SpawnCellRequest sr;
    sr.argv = std::move(argv);
    sr.cwd = cwd.path;
    sr.envp = BuildEnv(env_arg);
    sr.conf = ruleset_.conf.get();
    sr.seccomp = PlanFor(policy_);
    sr.limits = DefaultLimits();

    SpawnCellResult sp = SpawnCell(sr);
    if (!sp.ok) return MakeError(r.id, err::kInternal, sp.error);

    c = cells_.Create(cells_.NextId());
    c->proc = sp.proc;
    c->out_fd = sp.out_fd;
    c->err_fd = sp.err_fd;
    c->in_fd = sp.in_fd;
  }

  c->kill_deadline_ms = timeout_ms > 0 ? NowMs() + timeout_ms : 0;
  for (io::Fd fd : {c->out_fd, c->err_fd}) {
    if (fd == io::kInvalid) continue;
    reactor_.AddRead(fd);
  }

  return MakeOk(r.id, json{{"cell", c->id}, {"pty", c->is_pty}});
}

std::optional<json> Engine::OpExecStdin(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;

  const auto cell_it = r.args.find("cell");
  if (cell_it == r.args.end() || !cell_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "cell must be a string");
  }
  Cell* c = cells_.Find(cell_it->get<std::string>());
  if (c == nullptr) return MakeError(r.id, err::kNoCell, "no such cell");
  if (c->in_fd == io::kInvalid) return MakeError(r.id, err::kNoCell, "cell has no open stdin");

  const auto data_it = r.args.find("data");
  if (data_it == r.args.end() || !data_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "data must be a string");
  }
  const std::string data = data_it->get<std::string>();

  size_t off = 0;
  while (off < data.size()) {
    const long n = io::Write(c->in_fd, data.data() + off, data.size() - off);
    if (n == io::kWouldBlock) break;  // 缓冲满，如实回报写了多少
    if (n == io::kIoError) return MakeError(r.id, err::kInternal, "write to cell stdin failed");
    off += static_cast<size_t>(n);
  }

  // 可选地关闭 stdin（管道版用来给 `cat` 之类送 EOF）
  if (r.args.value("close", false) && !c->is_pty) {
    io::Close(c->in_fd);
    c->in_fd = io::kInvalid;
  }
  return MakeOk(r.id, json{{"written", off}, {"pending", data.size() - off}});
}

std::optional<json> Engine::OpExecWait(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;

  const auto cell_it = r.args.find("cell");
  if (cell_it == r.args.end() || !cell_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "cell must be a string");
  }
  Cell* c = cells_.Find(cell_it->get<std::string>());
  if (c == nullptr) return MakeError(r.id, err::kNoCell, "no such cell");

  int64_t yield_ms = 1000;
  if (auto it = r.args.find("yield_ms"); it != r.args.end() && it->is_number_integer()) {
    yield_ms = it->get<int64_t>();
  }
  size_t max_bytes = 64u * 1024u;
  if (auto it = r.args.find("max_bytes"); it != r.args.end() && it->is_number_unsigned()) {
    max_bytes = it->get<size_t>();
  }

  // 有数据或已结束 → 立即答复；否则挂起到 yield_ms（KeWaitForSingleObject 带超时）
  if (!c->buf.empty() || c->Finished()) {
    return BuildWaitReply(r.id, c, max_bytes);
  }
  waits_.push_back(PendingWait{r.id, c->id, NowMs() + yield_ms, max_bytes});
  return std::nullopt;
}

json Engine::BuildWaitReply(const std::string& req_id, Cell* c, size_t max_bytes) {
  std::string data;
  bool truncated = false;
  if (c->buf.size() > max_bytes) {
    data = c->buf.substr(0, max_bytes);
    c->buf.erase(0, max_bytes);
    truncated = true;
  } else {
    data.swap(c->buf);
  }

  const bool done = c->Finished();
  json result{{"done", done},
              {"data", data},
              {"truncated", truncated},
              {"dropped_bytes", c->dropped}};
  if (done) {
    result["exit_code"] = c->exit_code;
    result["signal"] = c->term_signal;
    result["timed_out"] = c->killed_by_timeout;
  } else {
    result["exit_code"] = nullptr;
  }

  json reply = MakeOk(req_id, std::move(result));
  if (done && c->buf.empty()) cells_.Erase(c->id);  // 取完即回收
  return reply;
}

std::optional<json> Engine::OpExecKill(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;

  const auto cell_it = r.args.find("cell");
  if (cell_it == r.args.end() || !cell_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "cell must be a string");
  }
  Cell* c = cells_.Find(cell_it->get<std::string>());
  if (c == nullptr) return MakeError(r.id, err::kNoCell, "no such cell");

  std::string signame = "TERM";
  if (auto it = r.args.find("signal"); it != r.args.end() && it->is_string()) {
    signame = it->get<std::string>();
  }
  Sig sig{};
  if (!ParseSignal(signame, &sig)) {
    return MakeError(r.id, err::kBadArgs, "unsupported signal: " + signame);
  }

  // 信号打的是整棵子树，不是单个进程。
  // KILL 走「收掉整组」那条路，否则杀不干净会 fork 的目标。
  bool killed = false;
  if (c->proc.valid()) {
    if (sig == Sig::kKill) {
      ScheduleKill(c->proc);
      killed = true;
    } else {
      // ★ 送不到就如实返回 false，绝不悄悄升级成强杀。
      //   Windows 上只有控制台程序会响应 CTRL_BREAK（见 proc_win.cpp），
      //   把「优雅终止」偷换成「强杀」会让调用方以为进程有机会清理，而它没有。
      killed = SignalGroup(c->proc, sig);
    }
  }
  return MakeOk(r.id, json{{"killed", killed}});
}

// ------------------------------------------------------------------ fs
//
// ★ 注意：hxd 自己不在沙箱里（它就是"内核"），所以 Landlock 拦不住这些 op。
//   path_guard 是唯一阻止宿主用 fs.read 读走 /etc/shadow 的东西。

std::optional<json> Engine::OpFsRead(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;

  const auto path_it = r.args.find("path");
  if (path_it == r.args.end() || !path_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "path must be a string");
  }
  const std::string rel = path_it->get<std::string>();
  const ResolveResult res =
      ResolveInRoots(rel, policy_.roots.front(), policy_.roots, /*must_exist=*/true);
  if (!res.ok) {
    Emit(MakeEvent("policy.violation",
                   json{{"op", "fs.read"}, {"path", rel}, {"reason", res.error}}));
    return MakeError(r.id, err::kPathEscape, res.error);
  }

  size_t offset = 0;
  size_t limit = 2000;
  if (auto it = r.args.find("offset"); it != r.args.end() && it->is_number_unsigned()) {
    offset = it->get<size_t>();
  }
  if (auto it = r.args.find("limit"); it != r.args.end() && it->is_number_unsigned()) {
    limit = it->get<size_t>();
  }

  std::FILE* f = platform::FopenUtf8(res.path, "rb");
  if (f == nullptr) {
    return MakeError(r.id, err::kBadArgs, "open failed: " + res.path);
  }
  std::string content;
  size_t line_no = 0;
  size_t emitted = 0;
  bool truncated = false;
  std::string line;
  int ch;
  while ((ch = std::fgetc(f)) != EOF) {
    line.push_back(static_cast<char>(ch));
    if (ch != 0x0a) continue;
    if (line_no >= offset) {
      if (emitted >= limit) { truncated = true; break; }
      content += line;
      ++emitted;
    }
    ++line_no;
    line.clear();
  }
  if (!truncated && !line.empty() && line_no >= offset && emitted < limit) {
    content += line;
    ++emitted;
    ++line_no;
  }
  std::fclose(f);

  return MakeOk(r.id, json{{"content", content},
                           {"lines", emitted},
                           {"next_offset", offset + emitted},
                           {"truncated", truncated}});
}

std::optional<json> Engine::OpFsApplyPatch(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;

  const auto p_it = r.args.find("patch");
  if (p_it == r.args.end() || !p_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "patch must be a string");
  }

  const ApplyPatchResult res = ApplyPatch(p_it->get<std::string>(), policy_.roots.front(),
                                          policy_.roots, WriteAllowed());
  if (!res.ok) {
    if (res.error_code == err::kPathEscape || res.error_code == err::kDenied) {
      Emit(MakeEvent("policy.violation",
                     json{{"op", "fs.apply_patch"}, {"reason", res.error}}));
    }
    return MakeError(r.id, res.error_code, res.error);
  }

  json changes = json::array();
  for (const auto& c : res.changes) {
    changes.push_back(json{{"path", c.path}, {"kind", c.kind}});
  }
  return MakeOk(r.id, json{{"changes", std::move(changes)}});
}

std::optional<json> Engine::OpFsStat(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;
  const auto path_it = r.args.find("path");
  if (path_it == r.args.end() || !path_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "path must be a string");
  }
  const std::string rel = path_it->get<std::string>();
  const ResolveResult res =
      ResolveInRoots(rel, policy_.roots.front(), policy_.roots, /*must_exist=*/false);
  if (!res.ok) return MakeError(r.id, err::kPathEscape, res.error);

  platform::StatInfo st;
  if (!platform::Stat(res.path, &st)) {
    return MakeOk(r.id, json{{"exists", false}});
  }
  const char* kind = st.is_dir ? "dir" : (st.is_regular ? "file" : "other");
  return MakeOk(r.id, json{{"exists", true},
                           {"kind", kind},
                           {"size", st.size},
                           // Windows 没有 POSIX 权限位，这里是合成值（见 platform::Stat）。
                           // 协议里它只用于展示，不参与任何决策。
                           {"mode", st.mode},
                           {"mtime_ms", st.mtime_ms}});
}

std::optional<json> Engine::OpFsGlob(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;
  const auto pat_it = r.args.find("pattern");
  if (pat_it == r.args.end() || !pat_it->is_string()) {
    return MakeError(r.id, err::kBadArgs, "pattern must be a string");
  }
  const std::string pattern = pat_it->get<std::string>();
  size_t limit = 1000;
  if (auto it = r.args.find("limit"); it != r.args.end() && it->is_number_unsigned()) {
    limit = it->get<size_t>();
  }

  // 按路径分量匹配。用整串 fnmatch 有两种都错的选法：
  //   不带 FNM_PATHNAME —— "*.txt" 会匹配到子目录里的文件
  //   带  FNM_PATHNAME —— "**" 失去"零个或多个目录"的含义
  // 所以分量级匹配才是对的：'**' 吃掉任意多层，其余分量各自 fnmatch。
  const std::vector<std::string> pat_parts = SplitComponents(pattern);
  json matches = json::array();
  bool truncated = false;
  const std::string& root = policy_.roots.front();

  std::vector<std::string> stack{root};
  while (!stack.empty() && !truncated) {
    const std::string dir = stack.back();
    stack.pop_back();
    std::vector<std::string> names;
    // 读不动的目录直接跳过：一个权限不足的子目录不该让整个 glob 失败。
    if (!platform::ListDir(dir, &names)) continue;
    for (const auto& name : names) {
      const std::string full = platform::Join(dir, name);
      const std::string rel = full.substr(root.size() + 1);
      platform::StatInfo st;
      if (!platform::Stat(full, &st)) continue;
      if (st.is_dir) {
        stack.push_back(full);
        continue;
      }
      if (MatchComponents(pat_parts, 0, SplitComponents(rel), 0)) {
        if (matches.size() >= limit) { truncated = true; break; }
        // ★ 回给宿主的一律用 '/'。协议是跨平台的，宿主（以及模型）
        //   拿到的路径不该因为引擎跑在哪个系统上而变形。
        std::string norm = rel;
        for (char& ch : norm) {
          if (platform::IsSeparator(ch)) ch = '/';
        }
        matches.push_back(norm);
      }
    }
  }
  return MakeOk(r.id, json{{"matches", std::move(matches)}, {"truncated", truncated}});
}

// ------------------------------------------------------------------ log

std::optional<json> Engine::OpLogAppend(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;
  const auto rec = r.args.find("record");
  if (rec == r.args.end() || !rec->is_object()) {
    return MakeError(r.id, err::kBadArgs, "record must be an object");
  }
  const int64_t seq = rollout_.Append(*rec);
  if (seq < 0) return MakeError(r.id, err::kInternal, "rollout append failed");
  return MakeOk(r.id, json{{"seq", seq}});
}

std::optional<json> Engine::OpLogFlush(const Request& r) {
  json err;
  if (!RequireSession(r, &err)) return err;
  return MakeOk(r.id, json{{"flushed", rollout_.Flush()}});
}

// ------------------------------------------------------------------ 循环

void Engine::OnLine(std::string_view line) {
  if (line.empty()) return;
  Request req;
  ParseError perr;
  if (!ParseRequest(std::string(line), &req, &perr)) {
    if (!perr.id.empty()) {
      Emit(MakeError(perr.id, perr.code, perr.message));
    } else {
      Emit(MakeEvent("engine.warning",
                     json{{"message", perr.message}, {"detail", json{{"code", perr.code}}}}));
    }
    return;
  }
  auto it = ops_.find(req.op);
  if (it == ops_.end()) {
    Reply(MakeError(req.id, err::kUnknownOp, "unknown op: " + req.op));
    return;
  }
  std::optional<json> reply = it->second(req);
  if (reply.has_value()) Reply(*reply);  // nullopt = 延后答复
}

int Engine::ComputeTimeoutMs() const {
  int64_t next = -1;
  auto consider = [&next](int64_t deadline) {
    if (deadline <= 0) return;
    if (next < 0 || deadline < next) next = deadline;
  };
  for (const auto& w : waits_) consider(w.deadline_ms);
  for (const auto& k : pending_kills_) consider(k.next_sweep_ms);
  for (const auto& [id, c] : cells_.all()) {
    consider(c->kill_deadline_ms);
    if (c->exited && (c->out_fd != io::kInvalid || c->err_fd != io::kInvalid)) {
      consider(c->fd_close_deadline_ms != 0 ? c->fd_close_deadline_ms : NowMs() + 50);
    }
    // 管道已 EOF 但还没 reap：短轮询等子进程退出
    if (!c->exited && c->out_fd == io::kInvalid && c->err_fd == io::kInvalid) {
      consider(NowMs() + 20);
    }
  }
  if (next < 0) return -1;
  const int64_t delta = next - NowMs();
  return delta <= 0 ? 0 : static_cast<int>(delta);
}

void Engine::CloseCellFd(Cell* c, io::Fd fd) {
  reactor_.Del(fd);
  // ★ 先摘引用再关。
  //   Linux 的 pty 主端读写是同一个 fd，in_fd 与 out_fd 会指向同一个东西，
  //   不先置空就会二次关闭。ConPTY 那边是两条独立管道，这一行不触发 ——
  //   两种情况用同一段代码是对的，因为判断条件问的是「是不是同一个」，
  //   而不是「是不是 pty」。
  if (c->in_fd == fd) c->in_fd = io::kInvalid;
  io::Close(fd);
  if (c->out_fd == fd) c->out_fd = io::kInvalid;
  if (c->err_fd == fd) c->err_fd = io::kInvalid;
}

void Engine::OnCellFdReadable(io::Fd fd) {
  Cell* c = cells_.FindByFd(fd);
  if (c == nullptr) {
    reactor_.Del(fd);
    io::Close(fd);
    return;
  }
  const char* stream = c->is_pty ? "pty" : (fd == c->out_fd ? "stdout" : "stderr");

  // ★ 每次事件最多读这么多轮就让出去。
  //   原来是个无界 while(true)：只要子进程产出的速度超过我们读的速度，
  //   这个函数就永不返回，于是 EnforceDeadlines/ReapChildren 全被饿死 ——
  //   fork 炸弹因此杀不掉，超时也不生效。Reactor 保证水平触发语义
  //   （见 io.hpp），让出去之后下一轮自然会再被叫回来。
  constexpr int kMaxChunksPerEvent = 8;

  char buf[65536];
  for (int i = 0; i < kMaxChunksPerEvent; ++i) {
    const long n = io::Read(fd, buf, sizeof(buf));
    if (n > 0) {
      const size_t before = c->dropped;
      c->Append(buf, static_cast<size_t>(n));
      // 缓冲已满时不再往事件流里灌 —— 否则失控的输出会连宿主一起冲垮
      if (c->dropped == before) {
        Emit(MakeEvent("exec.output",
                       json{{"cell", c->id},
                            {"stream", stream},
                            {"data", std::string(buf, static_cast<size_t>(n))}}));
      }
      continue;
    }
    if (n == 0) {  // EOF
      CloseCellFd(c, fd);
      return;
    }
    if (n == io::kWouldBlock) return;
    CloseCellFd(c, fd);  // kIoError
    return;
  }
}

void Engine::ReapChildren() {
  for (auto& [id, c] : cells_.all()) {
    if (c->exited || !c->proc.valid()) continue;
    int code = -1;
    int sig = 0;
    if (!Reap(&c->proc, &code, &sig)) continue;
    c->exited = true;
    c->exit_code = code;
    c->term_signal = sig;
    // ★ cell 拥有它的进程组：直接子进程退了，组里若还有成员，必须收掉。
    //
    //   `bash -c ':(){ :|:& };:'` 的顶层 bash 会立刻正常退出（exit_code=0），
    //   把子孙留成孤儿。超时分支因为 c->exited 已为真而不会触发，于是那些进程
    //   永远没人管。这不只是 fork 炸弹的问题 —— 任何 `cmd &` / 启后台服务的
    //   命令都会这样漏出去。
    //
    //   ★ 问的是 GroupAlive（整棵子树里还有没有活的），不是「直接子进程还在吗」。
    //     Windows 上这是 Job 的 ActiveProcesses，Linux 上是 kill(-pgid, 0)。
    bool orphans = false;
    if (GroupAlive(c->proc)) {
      orphans = true;
      ScheduleKill(c->proc);
    }
    Emit(MakeEvent("exec.exit", json{{"cell", c->id},
                                     {"exit_code", c->exit_code},
                                     {"signal", c->term_signal},
                                     {"timed_out", c->killed_by_timeout},
                                     {"orphans_killed", orphans}}));
  }
}

void Engine::ScheduleKill(const Proc& p) {
  if (!p.valid()) return;
  FreezeAndKillGroup(p);
  // ★ 要不要排清扫，由平台说了算（见 DupGroupForSweep）：
  //   Linux 要 —— SIGKILL 和 fork 在赛跑，而且 cell 马上会被 exec.wait 回收，
  //               清理必须活得比它久。
  //   Windows 不要 —— TerminateJobObject 是原子的，没有幸存者可扫。
  Proc dup;
  if (DupGroupForSweep(p, &dup)) {
    pending_kills_.push_back(PendingKill{dup, 12, NowMs() + 150});
  }
}

void Engine::SweepKills() {
  const int64_t now = NowMs();
  for (auto it = pending_kills_.begin(); it != pending_kills_.end();) {
    if (now < it->next_sweep_ms) {
      ++it;
      continue;
    }
    // 组空了就收工
    if (!GroupAlive(it->proc)) {
      ReleaseProc(&it->proc);
      it = pending_kills_.erase(it);
      continue;
    }
    FreezeAndKillGroup(it->proc);
    if (--it->sweeps_left <= 0) {
      // 扫了这么多轮还在，如实上报，不假装干净了
      Emit(MakeEvent("engine.warning",
                     json{{"message", "process group survived cleanup sweeps"},
                          {"detail", json{{"pgid", it->proc.pid}}}}));
      ReleaseProc(&it->proc);
      it = pending_kills_.erase(it);
      continue;
    }
    it->next_sweep_ms = now + 150;
    ++it;
  }
}

void Engine::EnforceDeadlines() {
  const int64_t now = NowMs();
  for (auto& [id, c] : cells_.all()) {
    // ① 超时：开始清扫
    if (!c->exited && c->kill_deadline_ms != 0 && now >= c->kill_deadline_ms) {
      c->killed_by_timeout = true;
      c->kill_deadline_ms = 0;
      ScheduleKill(c->proc);
    }

    // ③ 收口：直接子进程已退出，但 fd 还被孙进程占着 —— EOF 不会来了
    if (c->exited && (c->out_fd != io::kInvalid || c->err_fd != io::kInvalid)) {
      if (c->fd_close_deadline_ms == 0) {
        c->fd_close_deadline_ms = now + 300;  // 先把缓冲里的输出读干净
      } else if (now >= c->fd_close_deadline_ms) {
        if (c->out_fd != io::kInvalid) CloseCellFd(c.get(), c->out_fd);
        if (c->err_fd != io::kInvalid) CloseCellFd(c.get(), c->err_fd);
      }
    }
  }
}

void Engine::ServicePendingWaits() {
  const int64_t now = NowMs();
  for (auto it = waits_.begin(); it != waits_.end();) {
    Cell* c = cells_.Find(it->cell_id);
    if (c == nullptr) {
      Reply(MakeError(it->req_id, err::kNoCell, "cell disappeared while waiting"));
      it = waits_.erase(it);
      continue;
    }
    if (!c->buf.empty() || c->Finished() || now >= it->deadline_ms) {
      Reply(BuildWaitReply(it->req_id, c, it->max_bytes));
      it = waits_.erase(it);
      continue;
    }
    ++it;
  }
}

int Engine::Run() {
  std::string err;
  // stdio 先接管：宿主断开时不能被信号杀掉，stdout 也必须非阻塞
  // （否则读得慢的宿主能把整个单线程引擎冻住，见 Engine::Send 的注释）。
  if (!io::InitStdio(&err)) {
    ::fprintf(stderr, "hxd: stdio: %s\n", err.c_str());
    return 1;
  }
  if (!reactor_.Open(&err)) {
    ::fprintf(stderr, "hxd: reactor: %s\n", err.c_str());
    return 1;
  }
  if (!reactor_.AddRead(io::kStdin)) {
    ::fprintf(stderr, "hxd: reactor: cannot watch stdin\n");
    return 1;
  }

  LineReader reader;
  const auto on_line = [this](std::string_view l) { OnLine(l); };
  const auto on_oversize = [this]() {
    Emit(MakeEvent("engine.warning", json{{"message", "line exceeds limit; discarded"},
                                          {"detail", json{{"code", err::kLineTooLong},
                                                          {"limit_bytes", kMaxLineBytes}}}}));
  };

  constexpr int kMaxEvents = 32;
  io::Event events[kMaxEvents];
  char buf[65536];
  bool stdin_open = true;

  while (running_) {
    const int n = reactor_.Wait(ComputeTimeoutMs(), events, kMaxEvents, &err);
    if (n < 0) {
      ::fprintf(stderr, "hxd: reactor: %s\n", err.c_str());
      return 1;
    }

    for (int i = 0; i < n; ++i) {
      const io::Fd fd = events[i].fd;
      if (fd == io::kStdout) {
        if (events[i].writable) FlushOut();
        continue;
      }
      if (fd == io::kStdin) {
        if (!events[i].readable) continue;
        const long got = io::Read(io::kStdin, buf, sizeof(buf));
        if (got > 0) {
          reader.Feed(std::string_view(buf, static_cast<size_t>(got)), on_line, on_oversize);
        } else if (got == 0 || got == io::kIoError) {
          // 宿主关掉了 stdin（或它没了）。不立刻退出：还没答复的请求
          // 与还在跑的 cell 必须先收尾，见循环末尾的判断。
          stdin_open = false;
          reactor_.Del(io::kStdin);
        }
      } else if (events[i].readable) {
        OnCellFdReadable(fd);
      }
    }

    EnforceDeadlines();
    SweepKills();
    ReapChildren();
    ServicePendingWaits();
    FlushOut();

    // stdin 关闭且没有待答复的请求、没有活着的 cell → 干净退出
    if (!stdin_open && waits_.empty() && pending_kills_.empty()) {
      bool any_alive = false;
      for (auto& [id, c] : cells_.all()) {
        if (!c->Finished()) { any_alive = true; break; }
      }
      if (!any_alive) running_ = false;
    }
  }
  return 0;
}

}  // namespace hx
