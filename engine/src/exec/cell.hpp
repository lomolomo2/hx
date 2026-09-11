// Cell —— 一个受沙箱约束的子进程。
//
// "cell" 是协议里的不透明 ID：真实 pid 从不出现在协议中（HANDLE 模型，
// 见 harness-windows-kernel-analogy.md §2.3）。这样将来把实现换成容器
// 或远程机器，宿主和模型侧都不用改。
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hx {

// 单个 cell 的输出上限；超出部分丢弃并计数（截断是第一天就要有的东西）
inline constexpr size_t kCellBufferCap = 4u * 1024u * 1024u;

struct Cell {
  std::string id;
  pid_t pid = -1;
  int out_fd = -1;  // 子进程 stdout 的读端
  int err_fd = -1;  // 子进程 stderr 的读端

  std::string buf;        // 尚未被 exec.wait 取走的输出
  size_t dropped = 0;     // 因超上限被丢弃的字节数
  bool is_pty = false;  // pty 只有一个 fd，输出不分 stdout/stderr
  int in_fd = -1;       // 管道版的 stdin 写端；pty 版与 out_fd 同为主端
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

  bool Finished() const { return exited && out_fd < 0 && err_fd < 0; }
  void Append(const char* data, size_t len);
};

class CellTable {
 public:
  Cell* Create(const std::string& id);
  Cell* Find(const std::string& id);
  Cell* FindByFd(int fd);
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
