#include "log/rollout.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace hx {
namespace {

bool MkdirP(const std::string& path, std::string* err) {
  std::string acc;
  size_t i = 0;
  while (i < path.size()) {
    const size_t slash = path.find('/', i + 1);
    acc = (slash == std::string::npos) ? path : path.substr(0, slash);
    if (::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) {
      *err = "mkdir " + acc + ": " + ::strerror(errno);
      return false;
    }
    if (slash == std::string::npos) break;
    i = slash;
  }
  return true;
}

std::string TwoDigit(int v) {
  char b[16];
  ::snprintf(b, sizeof(b), "%02d", v);
  return b;
}

int64_t NowUnixMs() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

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
    const char* home = ::getenv("HOME");
    if (home == nullptr) {
      *err = "HOME is not set and no base given";
      return false;
    }
    root = std::string(home) + "/.hx/sessions";
  }

  const time_t now = ::time(nullptr);
  struct tm tm_buf {};
  ::localtime_r(&now, &tm_buf);
  const std::string dir = root + "/" + std::to_string(1900 + tm_buf.tm_year) + "/" +
                          TwoDigit(tm_buf.tm_mon + 1) + "/" + TwoDigit(tm_buf.tm_mday);
  if (!MkdirP(dir, err)) return false;

  path_ = dir + "/rollout-" + name + ".jsonl";
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    *err = "open " + path_ + ": " + ::strerror(errno);
    return false;
  }
  return true;
}

int64_t Rollout::Append(const json& record) {
  if (fd_ < 0) return -1;

  json line = record;
  line["seq"] = ++seq_;
  line["ts"] = NowUnixMs();

  std::string s = line.dump();
  s.push_back('\n');

  // 一次 write() 追加：进程中途被杀也不会留下半行。
  // 短写在常规文件上极罕见，但真发生了必须如实上报，不能假装成功。
  const ssize_t n = ::write(fd_, s.data(), s.size());
  if (n < 0 || static_cast<size_t>(n) != s.size()) {
    return -1;
  }
  return seq_;
}

bool Rollout::Flush() { return fd_ >= 0 && ::fsync(fd_) == 0; }

void Rollout::Close() {
  if (fd_ >= 0) {
    ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace hx
