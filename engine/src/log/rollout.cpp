#include "log/rollout.hpp"

#include "platform/platform.hpp"

#include <cstdio>

namespace hx {
namespace {

int64_t NowUnixMs() { return platform::UnixNowMs(); }

}  // namespace

Rollout::~Rollout() { Close(); }

bool Rollout::IsValidName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool Rollout::Open(const std::string& base, const std::string& name, std::string* err) {
  if (!IsValidName(name)) {
    *err = "invalid session name (want [A-Za-z0-9_-]{1,64})";
    return false;
  }

  std::string root = base;
  if (root.empty()) {
    const std::string home = platform::HomeDir();
    if (home.empty() || home == ".") {
      *err = "no home directory and no base given";
      return false;
    }
    root = platform::Join(platform::Join(home, ".hx"), "sessions");
  }

  // LocalDatePath returns the YYYY/MM/DD form, so a bare Join produces a path
  // mixing both separators (backslashes in the first half, forward slashes in
  // the second). It works, but it looks like a bug in the log and will not
  // match when compared against roots -- so normalize to this platform's
  // separator.
  const std::string dir = platform::Normalize(platform::Join(root, platform::LocalDatePath()));
  if (!platform::MakeDirs(dir, err)) return false;

  path_ = platform::Join(dir, "rollout-" + name + ".jsonl");
  return file_.Open(path_, err);
}

int64_t Rollout::Append(const json& record) {
  if (!file_.valid()) return -1;

  json line = record;
  line["seq"] = ++seq_;
  line["ts"] = NowUnixMs();

  std::string s = line.dump();
  s.push_back('\n');

  // One appending write: a process killed partway leaves no half line. See
  // platform::AppendFile.
  if (!file_.WriteRecord(s)) return -1;
  return seq_;
}

bool Rollout::Flush() { return file_.Sync(); }

void Rollout::Close() { file_.Close(); }

}  // namespace hx
