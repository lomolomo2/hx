// Engine —— hxd 的事件循环与操作分发。
//
// 单线程 reactor：stdin（宿主请求）+ 所有 cell 的流 + 定时器（wait 截止/进程超时）
// 都在同一个循环里。引擎状态只被这一个线程改，因此没有锁，也没有竞态。
//
// ★ 平台差异只藏在 io::Reactor 后面（Linux epoll / Windows IOCP），
//   下面这些逻辑一行都不该因为换了操作系统而分叉。
//   唯一的例外记在 io_win.cpp 顶部：Windows 上 stdio 由两个搬字节的线程驱动，
//   因为宿主给的标准句柄是同步的、进不了 IOCP。那两个线程不碰这里的任何状态。
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

// 尚未答复的 exec.wait。协议保证每个请求恰好一个响应，
// 所以这里必须记住 req_id，到期或有数据时补发。
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
  // 返回 nullopt 表示"延后答复"（目前只有 exec.wait 会这样）
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

  // 事件循环内部
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

  // ★ 出站必须是非阻塞的。
  //   早先 Emit 直接 write(stdout)：一旦宿主读得慢（或子进程输出爆量），
  //   管道写满，单线程引擎就阻塞在 write() 里 —— 超时不生效、进程不回收、
  //   整个事件循环冻结。一个慢速宿主就能让引擎停摆。
  //   现在：事件进出站队列，队列满了丢事件；响应永远不丢（协议契约是
  //   每个请求恰好一个响应）。
  void Emit(const json& j);          // 事件，可丢
  void Reply(const json& j);         // 响应，不可丢
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
   * 待清扫的进程组。
   *
   * ★ 故意不挂在 Cell 上：cell 一旦 done 就会被 exec.wait 回收，
   *   而进程树的清理往往要持续更久（fork 炸弹要扫很多轮）。
   *   把清理绑在汇报生命周期上，结果就是「cell 结束了、进程还在」。
   */
  struct PendingKill {
    Proc proc;  // 自己持有一份组句柄，与 cell 的生命周期解耦
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
  bool out_watched_ = false;   // stdout 是否已挂上「关注可写」
  uint64_t dropped_events_ = 0;
};

}  // namespace hx
