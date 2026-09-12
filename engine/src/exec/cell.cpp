#include "exec/cell.hpp"

#include "platform/platform.hpp"

namespace hx {

Cell::~Cell() {
  // out_fd / err_fd go through Engine::CloseCellFd (which also has to drop the
  // reactor registration); all that can still be open by this point is the
  // stdin write end.
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
