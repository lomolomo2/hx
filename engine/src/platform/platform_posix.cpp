#include "platform/platform.hpp"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace hx::platform {

int64_t NowMs() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t Pid() { return static_cast<int64_t>(::getpid()); }

bool RealPath(const std::string& in, std::string* out) {
  char* real = ::realpath(in.c_str(), nullptr);
  if (real == nullptr) return false;
  out->assign(real);
  ::free(real);
  return true;
}

bool IsDirectory(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsRegularFile(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool Stat(const std::string& p, StatInfo* out) {
  struct stat st {};
  if (::stat(p.c_str(), &st) != 0) {
    out->exists = false;
    return false;
  }
  out->exists = true;
  out->is_dir = S_ISDIR(st.st_mode);
  out->is_regular = S_ISREG(st.st_mode);
  out->size = static_cast<int64_t>(st.st_size);
  out->mode = static_cast<int>(st.st_mode & 07777);
  out->mtime_ms = static_cast<int64_t>(st.st_mtime) * 1000;
  return true;
}

bool MakeDirs(const std::string& path, std::string* err) {
  std::string acc;
  size_t i = 0;
  while (i < path.size()) {
    const size_t slash = path.find('/', i == 0 && path[0] == '/' ? 1 : i);
    acc = slash == std::string::npos ? path : path.substr(0, slash);
    if (!acc.empty() && ::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) {
      *err = "mkdir(" + acc + "): " + ::strerror(errno);
      return false;
    }
    if (slash == std::string::npos) break;
    i = slash + 1;
  }
  return true;
}

bool MakeTempDir(const std::string& prefix, std::string* out, std::string* err) {
  std::string tmpl = "/tmp/" + prefix + "XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) {
    *err = std::string("mkdtemp: ") + ::strerror(errno);
    return false;
  }
  out->assign(buf.data());
  return true;
}

bool RemoveDir(const std::string& path) { return ::rmdir(path.c_str()) == 0; }

std::string HomeDir() {
  const char* home = ::getenv("HOME");
  return home != nullptr ? std::string(home) : std::string(".");
}

bool GetEnv(const char* name, std::string* out) {
  const char* v = ::getenv(name);
  if (v == nullptr) return false;
  out->assign(v);
  return true;
}

bool ListDir(const std::string& dir, std::vector<std::string>* names) {
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return false;
  while (struct dirent* de = ::readdir(d)) {
    const std::string name = de->d_name;
    if (name == "." || name == "..") continue;
    names->push_back(name);
  }
  ::closedir(d);
  return true;
}

bool MatchComponent(const std::string& pattern, const std::string& name) {
  return ::fnmatch(pattern.c_str(), name.c_str(), FNM_PATHNAME) == 0;
}

char PreferredSeparator() { return '/'; }
bool IsSeparator(char c) { return c == '/'; }
bool IsAbsolute(const std::string& p) { return !p.empty() && p[0] == '/'; }

std::string Join(const std::string& base, const std::string& rel) {
  if (IsAbsolute(rel)) return rel;
  if (base.empty()) return rel;
  if (base.back() == '/') return base + rel;
  return base + "/" + rel;
}

std::string Normalize(const std::string& p) { return p; }

std::string Parent(const std::string& p) {
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return p.substr(0, slash);
}

// POSIX filesystems are case-sensitive, so folding is the identity.
std::string FoldCase(const std::string& p) { return p; }

bool IsWithin(const std::string& path, const std::string& root) {
  if (path.size() < root.size()) return false;
  if (path.compare(0, root.size(), root) != 0) return false;
  if (path.size() == root.size()) return true;
  return root == "/" || path[root.size()] == '/';
}

// ---- Files ----

bool ReadWholeFile(const std::string& path, std::string* out, std::string* err) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *err = std::string("open: ") + ::strerror(errno);
    return false;
  }
  out->clear();
  char buf[65536];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      out->append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    *err = std::string("read: ") + ::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& content, std::string* err) {
  const std::string dir = Parent(path);
  std::string tmpl = dir + "/.hx-tmp-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');

  const int fd = ::mkstemp(buf.data());
  if (fd < 0) {
    *err = std::string("mkstemp: ") + ::strerror(errno);
    return false;
  }
  const std::string tmp_path(buf.data());

  size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      *err = std::string("write: ") + ::strerror(errno);
      ::close(fd);
      ::unlink(tmp_path.c_str());
      return false;
    }
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    *err = std::string("fsync: ") + ::strerror(errno);
    ::close(fd);
    ::unlink(tmp_path.c_str());
    return false;
  }
  ::close(fd);
  ::chmod(tmp_path.c_str(), 0644);
  if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
    *err = std::string("rename: ") + ::strerror(errno);
    ::unlink(tmp_path.c_str());
    return false;
  }
  return true;
}

bool Unlink(const std::string& path) { return ::unlink(path.c_str()) == 0; }

std::FILE* FopenUtf8(const std::string& path, const char* mode) {
  return std::fopen(path.c_str(), mode);
}

AppendFile::~AppendFile() { Close(); }

bool AppendFile::Open(const std::string& path, std::string* err) {
  Close();
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    *err = "open " + path + ": " + ::strerror(errno);
    return false;
  }
  h_ = static_cast<std::intptr_t>(fd);
  return true;
}

bool AppendFile::WriteRecord(const std::string& s) {
  if (h_ == -1) return false;
  // One appending write(): a process killed partway leaves no half line.
  // Short writes are vanishingly rare on regular files, but if one does
  // happen it must be reported honestly rather than papered over.
  const ssize_t n = ::write(static_cast<int>(h_), s.data(), s.size());
  return n >= 0 && static_cast<size_t>(n) == s.size();
}

bool AppendFile::Sync() { return h_ != -1 && ::fsync(static_cast<int>(h_)) == 0; }

void AppendFile::Close() {
  if (h_ == -1) return;
  ::fsync(static_cast<int>(h_));
  ::close(static_cast<int>(h_));
  h_ = -1;
}

std::string LocalDatePath() {
  const time_t now = ::time(nullptr);
  struct tm tm_buf {};
  ::localtime_r(&now, &tm_buf);
  char buf[32];
  ::snprintf(buf, sizeof(buf), "%04d/%02d/%02d", 1900 + tm_buf.tm_year, tm_buf.tm_mon + 1,
             tm_buf.tm_mday);
  return buf;
}

int64_t UnixNowMs() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace hx::platform
