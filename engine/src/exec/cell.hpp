// Cell —— 一个受沙箱约束的子进程。
//
// "cell" 是协议里的不透明 ID：真实 pid 从不出现在协议中（HANDLE 模型，
// 见 harness-windows-kernel-analogy.md §2.3）。这样将来把实现换成容器
// 或远程机器，宿主和模型侧都不用改。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "exec/proc.hpp"
#include "io/io.hpp"

namespace hx {

// 单个 cell 的输出上限；超出部分丢弃并计数（截断是第一天就要有的东西）
inline constexpr size_t kCellBufferCap = 4u * 1024u * 1024u;

struct Cell {
  std::string id;
  Proc proc;
  io::Fd out_fd = io::kInvalid;  // 子进程 stdout 的读端
  io::Fd err_fd = io::kInvalid;  // 子进程 stderr 的读端

  std::string buf;        // 尚未被 exec.wait 取走的输出
  size_t dropped = 0;     // 因超上限被丢弃的字节数
  bool is_pty = false;           // pty 的输出不分 stdout/stderr
  io::Fd in_fd = io::kInvalid;   // stdin 写端。
                                 // ★ 别假设 pty 时 in_fd == out_fd：Linux 的 pty 主端
                                 //   确实是同一个 fd，ConPTY 却是**两条独立管道**。
                                 //   关流时必须分别处理，见 Engine::CloseCellFd。
  bool exited = false;
  int exit_code = -1;
  int term_signal = 0;
  int64_t kill_deadline_ms = 0;  // 0 = 无超时
  bool killed_by_timeout = false;

  // ---- 收尾用的两个状态（都是被 fork 炸弹逼出来的）----
  //
  // 直接子进程已退出、但管道 fd 仍被孙进程持有时，EOF 永远不会来。
  // 给一小段时间把缓冲读干净，然后强制收口，否则这个 cell 永远不会 done。
  int64_t fd_close_deadline_ms = 0;

  bool Finished() const {
    return exited && out_fd == io::kInvalid && err_fd == io::kInvalid;
  }
  void Append(const char* data, size_t len);

  /**
   * ★ cell 被 exec.wait 回收时，内核对象必须一起还回去。
   *   Windows 上尤其要紧：Job 句柄漏一个，KILL_ON_JOB_CLOSE 就永远不触发，
   *   那棵子树会一直活到 hxd 退出为止 —— 正是「进程组即所有权」要防的事。
   */
  ~Cell();
  Cell() = default;
  Cell(const Cell&) = delete;
  Cell& operator=(const Cell&) = delete;
};

class CellTable {
 public:
  Cell* Create(const std::string& id);
  Cell* Find(const std::string& id);
  Cell* FindByFd(io::Fd fd);
  void Erase(const std::string& id);

  std::map<std::string, std::unique_ptr<Cell>>& all() { return cells_; }
  const std::map<std::string, std::unique_ptr<Cell>>& all() const { return cells_; }
  std::string NextId() { return "c" + std::to_string(++seq_); }

 private:
  std::map<std::string, std::unique_ptr<Cell>> cells_;
  uint64_t seq_ = 0;
};

int64_t NowMs();

}  // namespace hx
