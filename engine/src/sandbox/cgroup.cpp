#ifndef _WIN32
#include "sandbox/cgroup.hpp"

#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

namespace hx {
namespace {

// 把 errno 拼进消息，调用方只需关心字符串。
std::string Err(const char* what) {
  return std::string(what) + ": " + ::strerror(errno);
}

// 读 /proc/self/cgroup，返回 cgroup v2（"0::"）那行的相对路径。
// 统一层级（v2 only）时路径就是 hxd 所在 cgroup 的相对位置。
std::string SelfCgroupPath() {
  std::ifstream f("/proc/self/cgroup");
  std::string line;
  while (std::getline(f, line)) {
    // 形如 "0::<path>"；v2 行以 "0::" 开头。
    if (line.rfind("0::", 0) == 0) {
      return line.substr(3);
    }
  }
  return std::string();
}

// 找到 hxd 所在的 cgroup 绝对路径。找不到返回空串。
std::string OwnCgroupDir() {
  const std::string rel = SelfCgroupPath();
  if (rel.empty()) {
    return std::string();
  }
  return "/sys/fs/cgroup" + rel;
}

// 向 cgroup 文件写一行。成功返回 true。
bool WriteFile(const std::string& path, const std::string& value, std::string* err) {
  std::ofstream f(path);
  if (!f) {
    *err = "open(" + path + "): " + ::strerror(errno);
    return false;
  }
  f << value;
  f.flush();
  if (!f) {
    *err = "write(" + path + "): " + ::strerror(errno);
    return false;
  }
  return true;
}

// 读 cgroup 文件全部内容（一行）。成功返回 true。
bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream f(path);
  if (!f) {
    return false;
  }
  std::getline(f, *out);
  return true;
}

// 子 cgroup 名带 pid，避免同一 scope 下多个 hxd 撞名。
// ★ 探测与实际会话必须用不同的名字：同名时探测的 rmdir 会把会话的 cgroup 一起删掉。
std::string ChildName(const char* kind) {
  char buf[64];
  ::snprintf(buf, sizeof(buf), "hx-%s-%d", kind, static_cast<int>(::getpid()));
  return buf;
}

// subtree_control 里是否已启用某控制器。
bool HasController(const std::string& parent, const char* name) {
  std::string cur;
  if (!ReadFile(parent + "/cgroup.subtree_control", &cur)) return false;
  return cur.find(name) != std::string::npos;
}

}  // namespace

bool Cgroup2Usable(std::string* path, std::string* reason) {
  // 1) 确认 /sys/fs/cgroup 是 cgroup2 文件系统。
  struct statfs sfs {};
  if (::statfs("/sys/fs/cgroup", &sfs) != 0) {
    *reason = "statfs(/sys/fs/cgroup) failed";
    return false;
  }
  if (static_cast<unsigned long>(sfs.f_type) != CGROUP2_SUPER_MAGIC) {
    *reason = "/sys/fs/cgroup is not cgroup2";
    return false;
  }

  // 2) 找到 hxd 所在的 cgroup（父）。
  const std::string parent = OwnCgroupDir();
  if (parent.empty()) {
    *reason = "cannot find own cgroup (no '0::' line in /proc/self/cgroup)";
    return false;
  }

  // 3) 真正走一遍：建子 cgroup → 写 pids.max / memory.max。
  //    子 cgroup 要能用控制器，前提是父的 subtree_control 里启用了它们。
  //    父若已有内部进程（普通用户会话的典型情况），启用会 EBUSY —— 这正是
  //    本函数要探测并如实报告的约束。
  const std::string child = parent + "/" + ChildName("probe");
  std::string err;

  // ★ 探测必须无副作用：记下我们「新启用」了哪些控制器，结束时还原。
  //   原实现在探测成功的机器上会把 +pids +memory 永久留在父 cgroup 上。
  //   只还原自己加的 —— 用户本来就启用着的不能动。
  const bool had_pids = HasController(parent, "pids");
  const bool had_memory = HasController(parent, "memory");
  const auto restore = [&]() {
    std::string ignored;
    if (!had_pids) WriteFile(parent + "/cgroup.subtree_control", "-pids", &ignored);
    if (!had_memory) WriteFile(parent + "/cgroup.subtree_control", "-memory", &ignored);
  };

  // 先试启用 pids 与 memory。若父已启用过，再写一次无害；
  // 若因内部进程 EBUSY，则说明这条链走不通。
  if (!WriteFile(parent + "/cgroup.subtree_control", "+pids +memory", &err)) {
    *reason = "enable subtree_control: " + err;
    return false;  // 没启用成功，无需还原
  }

  if (::mkdir(child.c_str(), 0755) != 0 && errno != EEXIST) {
    *reason = "mkdir(" + child + "): " + ::strerror(errno);
    restore();
    return false;
  }

  // 4) 验证两个限额文件真的存在、可写、且写后读回一致 —— "存在"不等于
  //    "控制器已下发"。用保守值：子 cgroup 的 memory.max 不能高于父，
  //    探测值太大可能假失败。
  if (!WriteFile(child + "/pids.max", "128", &err)) {
    ::rmdir(child.c_str());
    restore();
    *reason = "pids.max: " + err;
    return false;
  }
  if (!WriteFile(child + "/memory.max", "16777216", &err)) {
    ::rmdir(child.c_str());
    restore();
    *reason = "memory.max: " + err;
    return false;
  }
  // 读回确认限额真的生效（写成功但内核没接受的情况也能抓到）。
  std::string back;
  if (!ReadFile(child + "/pids.max", &back) || back != "128") {
    ::rmdir(child.c_str());
    restore();
    *reason = "pids.max readback: " + back;
    return false;
  }
  if (!ReadFile(child + "/memory.max", &back) || back != "16777216") {
    ::rmdir(child.c_str());
    restore();
    *reason = "memory.max readback: " + back;
    return false;
  }

  // 5) 探测成功：还原 subtree_control 并清理临时 cgroup。
  restore();
  if (::rmdir(child.c_str()) != 0 && errno != ENOENT) {
    // 清理失败不影响"可用"结论，但记下来更诚实。
    *reason = "usable (probe cgroup cleanup: " + Err("rmdir") + ")";
  } else {
    *reason = "ok";
  }
  *path = child;
  return true;
}

Cgroup2Session::~Cgroup2Session() {
  if (active_ && !path_.empty()) {
    // 子 cgroup 里若还有进程 rmdir 会 EBUSY；此时限额已失效（进程可能已跑完），
    // 留一个空目录给 systemd/内核回收比报错更好。
    if (::rmdir(path_.c_str()) != 0 && errno != ENOENT && errno != EBUSY) {
      // 静默：析构里不宜再抛。
    }
  }
}

bool Cgroup2Session::Create(const CgroupLimits& limits, std::string* err) {
  if (active_) {
    *err = "cgroup already created";
    return false;
  }

  const std::string parent = OwnCgroupDir();
  if (parent.empty()) {
    *err = "cannot find own cgroup";
    return false;
  }

  // 启用父的 pids + memory 控制器（幂等：已启用再写无害）。
  if (!WriteFile(parent + "/cgroup.subtree_control", "+pids +memory", err)) {
    return false;
  }

  const std::string child = parent + "/" + ChildName("session");
  if (::mkdir(child.c_str(), 0755) != 0 && errno != EEXIST) {
    *err = "mkdir(" + child + "): " + ::strerror(errno);
    return false;
  }

  // 只写非零的限额；0 表示该项不设限，就不动内核默认（max）。
  if (limits.max_pids > 0) {
    char v[32];
    ::snprintf(v, sizeof(v), "%llu", static_cast<unsigned long long>(limits.max_pids));
    if (!WriteFile(child + "/pids.max", v, err)) {
      ::rmdir(child.c_str());
      return false;
    }
  }
  if (limits.max_memory_bytes > 0) {
    char v[32];
    ::snprintf(v, sizeof(v), "%llu", static_cast<unsigned long long>(limits.max_memory_bytes));
    if (!WriteFile(child + "/memory.max", v, err)) {
      ::rmdir(child.c_str());
      return false;
    }
  }

  path_ = child;
  active_ = true;
  return true;
}

bool Cgroup2Session::AddPid(pid_t pid, std::string* err) {
  if (!active_) {
    *err = "cgroup not created";
    return false;
  }
  char v[32];
  ::snprintf(v, sizeof(v), "%d", static_cast<int>(pid));
  return WriteFile(path_ + "/cgroup.procs", v, err);
}

}  // namespace hx
#endif  // !_WIN32
