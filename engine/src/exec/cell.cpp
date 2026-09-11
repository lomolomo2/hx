#include "exec/cell.hpp"

#include <ctime>

namespace hx {

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

Cell* CellTable::FindByFd(int fd) {
  for (auto& [id, c] : cells_) {
    if (c->out_fd == fd || c->err_fd == fd) return c.get();
  }
  return nullptr;
}

void CellTable::Erase(const std::string& id) { cells_.erase(id); }

int64_t NowMs() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace hx
