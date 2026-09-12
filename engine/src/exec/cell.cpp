#include "exec/cell.hpp"

#include "platform/platform.hpp"

namespace hx {

Cell::~Cell() {
  // out_fd / err_fd 走 Engine::CloseCellFd（那里要同时摘掉 reactor 注册）；
  // 到这里只剩可能还开着的 stdin 写端。
  if (in_fd != io::kInvalid && in_fd != out_fd) io::Close(in_fd);
  ReleaseProc(&proc);
}

void Cell::Append(const char* data, size_t len) {
  if (buf.size() >= kCellBufferCap) {
    dropped += len;
    return;
  }
  const size_t room = kCellBufferCap - buf.size();
  const size_t take = len < room ? len : room;
  buf.append(data, take);
  dropped += len - take;
}

Cell* CellTable::Create(const std::string& id) {
  auto c = std::make_unique<Cell>();
  c->id = id;
  Cell* raw = c.get();
  cells_[id] = std::move(c);
  return raw;
}

Cell* CellTable::Find(const std::string& id) {
  auto it = cells_.find(id);
  return it == cells_.end() ? nullptr : it->second.get();
}

Cell* CellTable::FindByFd(io::Fd fd) {
  for (auto& [id, c] : cells_) {
    if (c->out_fd == fd || c->err_fd == fd) return c.get();
  }
  return nullptr;
}

void CellTable::Erase(const std::string& id) { cells_.erase(id); }

int64_t NowMs() { return platform::NowMs(); }

}  // namespace hx
