// Engine -- hxd's event loop and op dispatch.
//
// A single-threaded reactor: stdin (host requests), every cell's streams, and
// the timers (wait deadlines / process timeouts) all live in one loop. Engine
// state is mutated by this thread alone, so there are no locks and no races.
//
// ★ Platform differences hide entirely behind io::Reactor (Linux epoll /
//   Windows IOCP); not one line of the logic below should fork because the
//   operating system changed.
//   The sole exception is recorded at the top of io_win.cpp: on Windows stdio
//   is driven by two byte-shuffling threads, because the standard handles the
//   host provides are synchronous and cannot join an IOCP. Those two threads
//   touch none of the state here.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "exec/cell.hpp"
#include "exec/proc.hpp"
#include "io/io.hpp"
#include "log/rollout.hpp"
#include "proto.hpp"
#include "sandbox/caps.hpp"
#include "sandbox/confine.hpp"
#include "sandbox/policy.hpp"

namespace hx {

// An exec.wait that has not been answered yet. The protocol guarantees exactly
// one response per request, so the req_id has to be remembered here and the
// response sent later, on expiry or when data arrives.
struct PendingWait {
  std::string req_id;
  std::string cell_id;
  int64_t deadline_ms = 0;
  size_t max_bytes = 0;
};

class Engine {
 public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  int Run();

  const Caps& caps() const { return caps_; }

 private:
  // Returning nullopt means "answer later" (currently only exec.wait does)
  using Handler = std::function<std::optional<json>(const Request&)>;

  void RegisterOps();
  void OnLine(std::string_view line);

  std::optional<json> OpPing(const Request& r);
  std::optional<json> OpSelfTest(const Request& r);
  std::optional<json> OpSessionOpen(const Request& r);
  std::optional<json> OpPolicySet(const Request& r);
  std::optional<json> OpExecStart(const Request& r);
  std::optional<json> OpExecStdin(const Request& r);
  std::optional<json> OpExecWait(const Request& r);
  std::optional<json> OpExecKill(const Request& r);
  std::optional<json> OpFsRead(const Request& r);
  std::optional<json> OpFsApplyPatch(const Request& r);
  std::optional<json> OpFsStat(const Request& r);
  std::optional<json> OpFsGlob(const Request& r);
  std::optional<json> OpLogAppend(const Request& r);
  std::optional<json> OpLogFlush(const Request& r);

  bool WriteAllowed() const { return policy_.sandbox != SandboxMode::kReadOnly; }

  // Event loop internals
  int ComputeTimeoutMs() const;
  void OnCellFdReadable(io::Fd fd);
  void CloseCellFd(Cell* c, io::Fd fd);
  void ReapChildren();
  void EnforceDeadlines();
  void ScheduleKill(const Proc& p);
  void SweepKills();
  void ServicePendingWaits();
  json BuildWaitReply(const std::string& req_id, Cell* c, size_t max_bytes);

  bool RequireSession(const Request& r, json* err_out) const;
  std::vector<std::string> BuildEnv(const json& overrides) const;
  json EffectiveJson() const;

  // ★ The outbound path must be non-blocking.
  //   Emit used to write(stdout) directly: the moment the host read slowly (or
  //   a child produced a flood of output) the pipe filled, and the
  //   single-threaded engine blocked inside write() -- timeouts stopped
  //   firing, processes stopped being reaped, and the whole event loop froze.
  //   One slow host was enough to bring the engine to a halt.
  //   Now: events go into an outbound queue, and events are dropped when it is
  //   full; responses are never dropped (the protocol contract is exactly one
  //   response per request).
  void Emit(const json& j);          // an event; droppable
  void Reply(const json& j);         // a response; never dropped
  void Send(std::string line, bool droppable);
  void FlushOut();

  Caps caps_;
  std::map<std::string, Handler, std::less<>> ops_;

  bool session_open_ = false;
  std::string session_id_;
  Policy policy_;
  RulesetBuild ruleset_;
  std::string tmpdir_;

  /**
   * Process groups still to be swept.
   *
   * ★ Deliberately not hung off Cell: a cell is reaped by exec.wait as soon as
   *   it is done, while cleaning up the process tree often has to continue
   *   much longer (a fork bomb takes many sweeps). Tying cleanup to the
   *   reporting lifecycle produces exactly "the cell finished but the
   *   processes are still there".
   */
  struct PendingKill {
    Proc proc;  // holds its own copy of the group handle, decoupled from the cell's lifetime
    int sweeps_left;
    int64_t next_sweep_ms;
  };
  std::vector<PendingKill> pending_kills_;

  Rollout rollout_;
  CellTable cells_;
  std::vector<PendingWait> waits_;
  io::Reactor reactor_;
  bool running_ = true;

  std::string out_buf_;
  bool out_watched_ = false;   // whether "watch for writable" is on for stdout
  uint64_t dropped_events_ = 0;
};

}  // namespace hx
