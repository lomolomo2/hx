#ifndef _WIN32
#include "exec/rlimit.hpp"

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hx {
namespace {

bool SetOne(int resource, uint64_t value, const char* name, std::string* err) {
  if (value == 0) return true;
  struct rlimit rl {};
  if (::getrlimit(resource, &rl) != 0) {
    *err = std::string("getrlimit(") + name + "): " + ::strerror(errno);
    return false;
  }
  // 只收紧不放宽：硬上限本来更低时就照旧
  const rlim_t want = static_cast<rlim_t>(value);
  if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) return true;
  rl.rlim_cur = want;
  rl.rlim_max = want;
  if (::setrlimit(resource, &rl) != 0) {
    *err = std::string("setrlimit(") + name + "): " + ::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

uint64_t CountUserTasks(unsigned uid) {
  DIR* proc = ::opendir("/proc");
  if (proc == nullptr) return 0;

  uint64_t tasks = 0;
  while (struct dirent* de = ::readdir(proc)) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    char path[320];
    ::snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) continue;  // 进程可能刚退出

    char line[256];
    bool mine = false;
    unsigned long long threads = 1;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      if (std::strncmp(line, "Uid:", 4) == 0) {
        unsigned long long real_uid = 0;
        if (std::sscanf(line + 4, "%llu", &real_uid) == 1) mine = (real_uid == uid);
        if (!mine) break;
      } else if (std::strncmp(line, "Threads:", 8) == 0) {
        std::sscanf(line + 8, "%llu", &threads);
        break;
      }
    }
    std::fclose(f);
    if (mine) tasks += threads;
  }
  ::closedir(proc);
  return tasks;
}

Limits LimitsFor(uint64_t headroom) {
  Limits l;
  const uint64_t current = CountUserTasks(static_cast<unsigned>(::getuid()));
  // 数不出来就不设上限：猜低了整个沙箱不可用，而 fork 炸弹还有
  // timeout + kill(-pgid) 兜底。宁可少一道缓解，不可让工具全线失灵。
  l.max_processes = current > 0 ? current + headroom : 0;
  // 单个文件最大 2 GiB —— 防止 `yes > f` 之类把磁盘写满。
  l.max_file_bytes = 2ull * 1024 * 1024 * 1024;
  l.max_open_files = 4096;
  l.disable_core_dumps = true;
  return l;
}

Limits DefaultLimits() { return LimitsFor(); }

bool ApplyLimits(const Limits& l, std::string* err) {
  if (l.disable_core_dumps) {
    struct rlimit zero {};
    zero.rlim_cur = 0;
    zero.rlim_max = 0;
    ::setrlimit(RLIMIT_CORE, &zero);  // 失败无所谓，core dump 只是噪音
  }
  if (!SetOne(RLIMIT_NPROC, l.max_processes, "RLIMIT_NPROC", err)) return false;
  if (!SetOne(RLIMIT_FSIZE, l.max_file_bytes, "RLIMIT_FSIZE", err)) return false;
  if (!SetOne(RLIMIT_NOFILE, l.max_open_files, "RLIMIT_NOFILE", err)) return false;
  return true;
}

}  // namespace hx
#endif  // !_WIN32
