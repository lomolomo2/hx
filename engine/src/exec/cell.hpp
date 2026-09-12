// Cell -- one sandboxed child process.
//
// "cell" is an opaque ID in the protocol: a real pid never appears there (the
// HANDLE model; see harness-windows-kernel-analogy.md section 2.3). That way,
// swapping the implementation for a container or a remote machine later
// requires no change on the host or model side.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "exec/proc.hpp"
#include "io/io.hpp"

namespace hx {

// Output cap for a single cell; anything beyond it is dropped and counted
// (truncation is a day-one requirement, not a later addition)
inline constexpr size_t kCellBufferCap = 4u * 1024u * 1024u;

struct Cell {
  std::string id;
  Proc proc;
  io::Fd out_fd = io::kInvalid;  // read end of the child's stdout
  io::Fd err_fd = io::kInvalid;  // read end of the child's stderr

  std::string buf;        // output exec.wait has not collected yet
  size_t dropped = 0;     // bytes dropped for exceeding the cap
  bool is_pty = false;           // pty output does not separate stdout/stderr
  io::Fd in_fd = io::kInvalid;   // stdin write end.
                                 // ★ Do not assume in_fd == out_fd for a pty: on Linux
                                 //   the pty master really is the same fd, but ConPTY
                                 //   uses **two independent pipes**. They must be closed
                                 //   separately; see Engine::CloseCellFd.
  bool exited = false;
  int exit_code = -1;
  int term_signal = 0;
  int64_t kill_deadline_ms = 0;  // 0 = no timeout
  bool killed_by_timeout = false;

  // ---- Two pieces of teardown state (both forced on us by fork bombs) ----
  //
  // When the direct child has exited but the pipe fds are still held by
  // grandchildren, EOF never arrives. Allow a short window to drain the
  // buffer, then force closure, or this cell is never done.
  int64_t fd_close_deadline_ms = 0;

  bool Finished() const {
    return exited && out_fd == io::kInvalid && err_fd == io::kInvalid;
  }
  void Append(const char* data, size_t len);

  /**
   * ★ When exec.wait reaps a cell, the kernel objects must go back with it.
   *   This matters most on Windows: leak one Job handle and
   *   KILL_ON_JOB_CLOSE never fires, so that subtree lives until hxd exits --
   *   exactly what "the process group is ownership" exists to prevent.
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
