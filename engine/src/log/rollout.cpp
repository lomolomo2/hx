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

  // LocalDatePath 给的是 YYYY/MM/DD 形式，直接 Join 会拼出混着两种分隔符的
  // 路径（前半截反斜杠、后半截正斜杠）。能用，但日志里看着像 bug，
  // 而且拿去和 roots 比较时会对不上 —— 统一成本平台的分隔符。
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

  // 一次写追加：进程中途被杀也不会留下半行。见 platform::AppendFile。
  if (!file_.WriteRecord(s)) return -1;
  return seq_;
}

bool Rollout::Flush() { return file_.Sync(); }

void Rollout::Close() { file_.Close(); }

}  // namespace hx
