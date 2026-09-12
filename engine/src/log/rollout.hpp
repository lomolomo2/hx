// rollout -- the append-only session log, and this system's source of truth.
//
// ★ The engine decides the path; the host may only supply a validated session
//   name. Otherwise the host could use log.append to append to any file at all
//   (a shell's rc file, say), which would be an architectural back door -- the
//   only place the engine writes must be one it chose itself.
//
// The durability promise (stated precisely, not vaguely):
//   - each record is appended with one write() (O_APPEND), so even a process
//     killed with -9 leaves no half line
//   - power-loss durability requires fsync, which happens only in Flush()
#pragma once

#include "platform/platform.hpp"

#include <cstdint>
#include <string>

#include "proto.hpp"

namespace hx {

class Rollout {
 public:
  ~Rollout();

  // An empty base means $HOME/.hx/sessions. name must be [A-Za-z0-9_-]{1,64}.
  bool Open(const std::string& base, const std::string& name, std::string* err);
  // Returns this record's sequence number; -1 when not open
  int64_t Append(const json& record);
  bool Flush();
  void Close();

  const std::string& path() const { return path_; }
  bool is_open() const { return file_.valid(); }
  int64_t seq() const { return seq_; }

  static bool IsValidName(const std::string& name);

 private:
  platform::AppendFile file_;
  std::string path_;
  int64_t seq_ = 0;
};

}  // namespace hx
