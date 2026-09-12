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

// Glob matching: "**" in pat matches zero or more path components.
bool MatchComponents(const std::vector<std::string>& pat, size_t pi,
                     const std::vector<std::string>& path, size_t si) {
  while (pi < pat.size()) {
    if (pat[pi] == "**") {
      if (pi + 1 == pat.size()) return true;  // a trailing ** swallows everything left
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
  // ★ Both separators have to be recognized.
  //   Globs written by the host always use '/' (the protocol is
  //   cross-platform), while the relative paths the engine computes on Windows
  //   carry '\'. Splitting on '/' alone means "src/**/*.ts" never matches
  //   anything on Windows -- and silently, as zero results with no error, the
  //   hardest kind to track down.
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
  // ruleset_ and reactor_ look after their own resources (the former is a
  // shared_ptr, the latter has a destructor). All that is left here is the
  // session-private tmp -- and failing to remove it when non-empty is
  // deliberate: leaving a trace is far safer than quietly recursive-deleting a
  // directory that may hold the user's data.
  if (!tmpdir_.empty()) platform::RemoveDir(tmpdir_);
}

// Outbound queue cap. Past it, events start being dropped -- dropping events
// is far better than freezing the engine.
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
      // Cannot write any more: turn on "watch for writable" and continue when
      // it is, never wait here
      if (!out_watched_ && reactor_.WatchWrite(io::kStdout, true)) out_watched_ = true;
      return;
    }
    // stdout broke: the host is gone. Drop the backlog; the loop exits at
    // stdin EOF
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
              // ★ No longer exposes a platform-specific number like
              //   landlock_abi. What the host needs to decide is "was there a
              //   real sandbox this time", not "which kernel interface was
              //   used"; the latter is fully recorded in caps (written in the
              //   very first session_meta record).
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
        // Non-existent: skip it and record it honestly rather than letting it
        // stop the whole session from opening
        p.extra_read_paths.push_back(raw);
        continue;
      }
      p.extra_read_paths.push_back(std::move(resolved));
    }
  }

  // The session-private tmp: outside the workspace so it does not pollute the
  // repo, but explicitly granted write access
  std::string tmperr;
  if (!platform::MakeTempDir("hx-sess-", &p.tmpdir, &tmperr)) {
    return MakeError(r.id, err::kInternal, tmperr);
  }

  // ★ The old ruleset is reclaimed by its shared_ptr -- but *when* it is
  //   reclaimed carries meaning: on Windows, destroying a Confinement revokes
  //   the ACEs stamped on the roots and deletes the AppContainer profile. So
  //   the assignment has to happen before the old one is destroyed; reversed,
  //   it would revoke the ACEs the new session has just stamped (same root,
  //   different SID, but profile deletion goes by name).
  ruleset_ = BuildConfinement(p, caps_);
  policy_ = std::move(p);
  tmpdir_ = policy_.tmpdir;
  session_open_ = true;

  // rollout: the engine decides the path; the host may only supply a validated name
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
  // The very first record nails "did this run have a real sandbox" into the log
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
  // Stated honestly: cells already running carry the old ruleset, and the
  // kernel layer cannot tighten retroactively
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
    // ★ Do not write in_fd = out_fd. On Linux the two really are the same fd,
    //   but ConPTY uses two independent pipes -- see the comments on
    //   PtySpawnResult.
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
    if (n == io::kWouldBlock) break;  // buffer full; report honestly how much went out
    if (n == io::kIoError) return MakeError(r.id, err::kInternal, "write to cell stdin failed");
    off += static_cast<size_t>(n);
  }

  // Optionally close stdin (the pipe flavour uses this to send EOF to things
  // like `cat`)
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

  // Data available or already finished -> answer at once; otherwise suspend
  // until yield_ms (KeWaitForSingleObject with a timeout)
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
  if (done && c->buf.empty()) cells_.Erase(c->id);  // fully drained, so reap it
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

  // Signals target the whole subtree, not a single process.
  // KILL takes the "collect the entire group" path, or a target that forks
  // never gets cleaned up.
  bool killed = false;
  if (c->proc.valid()) {
    if (sig == Sig::kKill) {
      ScheduleKill(c->proc);
      killed = true;
    } else {
      // ★ Undeliverable returns false honestly and never silently escalates to
      //   a hard kill. On Windows only console programs respond to CTRL_BREAK
      //   (see proc_win.cpp), and substituting a hard kill for a graceful
      //   termination would let the caller believe the process had a chance to
      //   clean up when it did not.
      killed = SignalGroup(c->proc, sig);
    }
  }
  return MakeOk(r.id, json{{"killed", killed}});
}

// ------------------------------------------------------------------ fs
//
// ★ Note that hxd itself is not inside the sandbox (it *is* the "kernel"), so
//   Landlock stops none of these ops. path_guard is the only thing preventing
//   the host from reading /etc/shadow through fs.read.

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
                           // Windows has no POSIX permission bits, so this is a
                           // synthesized value (see platform::Stat). In the
                           // protocol it is display-only and feeds into no
                           // decision.
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

  // Match per path component. Running fnmatch over the whole string offers two
  // choices, both wrong:
  //   without FNM_PATHNAME -- "*.txt" matches files inside subdirectories
  //   with    FNM_PATHNAME -- "**" loses its "zero or more directories" meaning
  // So component-level matching is the right answer: '**' swallows any number
  // of levels and every other component gets its own fnmatch.
  const std::vector<std::string> pat_parts = SplitComponents(pattern);
  json matches = json::array();
  bool truncated = false;
  const std::string& root = policy_.roots.front();

  std::vector<std::string> stack{root};
  while (!stack.empty() && !truncated) {
    const std::string dir = stack.back();
    stack.pop_back();
    std::vector<std::string> names;
    // Skip directories that cannot be read: one subdirectory with insufficient
    // permissions should not fail the entire glob.
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
        // ★ Everything returned to the host uses '/'. The protocol is
        //   cross-platform, and the paths the host (and the model) receive
        //   should not change shape depending on which OS the engine runs on.
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

// ------------------------------------------------------------------ the loop

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
  if (reply.has_value()) Reply(*reply);  // nullopt = answer later
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
    // Pipes at EOF but not reaped yet: poll briefly for the child to exit
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
  // ★ Drop the reference before closing.
  //   On Linux the pty master is read and written through one fd, so in_fd and
  //   out_fd point at the same thing and failing to null it first causes a
  //   double close. On ConPTY they are two independent pipes and this line does
  //   not fire -- one piece of code covering both is correct, because the
  //   condition asks "is it the same one", not "is it a pty".
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

  // ★ At most this many rounds per event, then yield.
  //   This used to be an unbounded while(true): as long as the child produced
  //   output faster than we read it, this function never returned, and
  //   EnforceDeadlines/ReapChildren starved -- so a fork bomb could not be
  //   killed and timeouts did not fire. The Reactor guarantees
  //   level-triggered semantics (see io.hpp), so after yielding we are
  //   naturally called back on the next round.
  constexpr int kMaxChunksPerEvent = 8;

  char buf[65536];
  for (int i = 0; i < kMaxChunksPerEvent; ++i) {
    const long n = io::Read(fd, buf, sizeof(buf));
    if (n > 0) {
      const size_t before = c->dropped;
      c->Append(buf, static_cast<size_t>(n));
      // Stop feeding the event stream once the buffer is full -- otherwise
      // runaway output swamps the host as well
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
    // ★ A cell owns its process group: once the direct child exits, anything
    //   still in the group must be collected.
    //
    //   The top-level bash of `bash -c ':(){ :|:& };:'` exits normally and
    //   immediately (exit_code=0), leaving its descendants orphaned. The
    //   timeout branch does not fire because c->exited is already true, so
    //   nothing ever looks after those processes. And this is not only a fork
    //   bomb problem -- any `cmd &` or command that starts a background
    //   service leaks the same way.
    //
    //   ★ The question is GroupAlive (is anything in the subtree still alive),
    //     not "is the direct child still there". On Windows that is the Job's
    //     ActiveProcesses; on Linux it is kill(-pgid, 0).
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
  // ★ Whether to queue a sweep is the platform's call (see DupGroupForSweep):
  //   Linux does -- SIGKILL races fork, and the cell is about to be reaped by
  //                 exec.wait, so cleanup has to outlive it.
  //   Windows does not -- TerminateJobObject is atomic and there are no
  //                 survivors to sweep.
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
    // The group is empty, so we are done
    if (!GroupAlive(it->proc)) {
      ReleaseProc(&it->proc);
      it = pending_kills_.erase(it);
      continue;
    }
    FreezeAndKillGroup(it->proc);
    if (--it->sweeps_left <= 0) {
      // Still there after that many sweeps: report it honestly rather than
      // pretending things are clean
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
    // 1. Timeout: start cleaning up
    if (!c->exited && c->kill_deadline_ms != 0 && now >= c->kill_deadline_ms) {
      c->killed_by_timeout = true;
      c->kill_deadline_ms = 0;
      ScheduleKill(c->proc);
    }

    // 3. Teardown: the direct child has exited but the fds are still held by
    //    grandchildren -- EOF is never coming
    if (c->exited && (c->out_fd != io::kInvalid || c->err_fd != io::kInvalid)) {
      if (c->fd_close_deadline_ms == 0) {
        c->fd_close_deadline_ms = now + 300;  // drain the buffered output first
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
  // Take over stdio first: a disconnecting host must not kill us with a
  // signal, and stdout has to be non-blocking (otherwise a slow-reading host
  // can freeze the whole single-threaded engine; see the comments on
  // Engine::Send).
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
          // The host closed stdin (or is gone). Do not exit immediately:
          // unanswered requests and running cells have to be wound up first;
          // see the check at the end of the loop.
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

    // stdin closed, no requests awaiting an answer, no live cells -> exit cleanly
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
