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

// Fold errno into the message so callers only have to care about the string.
std::string Err(const char* what) {
  return std::string(what) + ": " + ::strerror(errno);
}

// Read /proc/self/cgroup and return the relative path from the cgroup v2
// ("0::") line. Under a unified hierarchy (v2 only) that path is hxd's own
// cgroup's relative location.
std::string SelfCgroupPath() {
  std::ifstream f("/proc/self/cgroup");
  std::string line;
  while (std::getline(f, line)) {
    // Shaped like "0::<path>"; the v2 line starts with "0::".
    if (line.rfind("0::", 0) == 0) {
      return line.substr(3);
    }
  }
  return std::string();
}

// Find the absolute path of hxd's own cgroup. Empty string when not found.
std::string OwnCgroupDir() {
  const std::string rel = SelfCgroupPath();
  if (rel.empty()) {
    return std::string();
  }
  return "/sys/fs/cgroup" + rel;
}

// Write one line to a cgroup file. Returns true on success.
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

// Read a cgroup file's entire contents (one line). Returns true on success.
bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream f(path);
  if (!f) {
    return false;
  }
  std::getline(f, *out);
  return true;
}

// The child cgroup's name carries the pid, so several hxd instances in one
// scope do not collide.
// ★ Probing and real sessions must use different names: sharing a name lets
//   the probe's rmdir delete the session's cgroup along with its own.
std::string ChildName(const char* kind) {
  char buf[64];
  ::snprintf(buf, sizeof(buf), "hx-%s-%d", kind, static_cast<int>(::getpid()));
  return buf;
}

// Whether a controller is already enabled in subtree_control.
bool HasController(const std::string& parent, const char* name) {
  std::string cur;
  if (!ReadFile(parent + "/cgroup.subtree_control", &cur)) return false;
  return cur.find(name) != std::string::npos;
}

}  // namespace

bool Cgroup2Usable(std::string* path, std::string* reason) {
  // 1) Confirm /sys/fs/cgroup is a cgroup2 filesystem.
  struct statfs sfs {};
  if (::statfs("/sys/fs/cgroup", &sfs) != 0) {
    *reason = "statfs(/sys/fs/cgroup) failed";
    return false;
  }
  if (static_cast<unsigned long>(sfs.f_type) != CGROUP2_SUPER_MAGIC) {
    *reason = "/sys/fs/cgroup is not cgroup2";
    return false;
  }

  // 2) Find hxd's own cgroup (the parent).
  const std::string parent = OwnCgroupDir();
  if (parent.empty()) {
    *reason = "cannot find own cgroup (no '0::' line in /proc/self/cgroup)";
    return false;
  }

  // 3) Actually walk it: create the child cgroup -> write pids.max /
  //    memory.max. For the child to use the controllers, the parent's
  //    subtree_control must have them enabled.
  //    If the parent already has internal processes (the typical case in an
  //    ordinary user session), enabling them gives EBUSY -- which is exactly
  //    the constraint this function exists to probe and report honestly.
  const std::string child = parent + "/" + ChildName("probe");
  std::string err;

  // ★ Probing must have no side effects: record which controllers *we* newly
  //   enabled and restore them at the end. The original implementation left
  //   +pids +memory permanently on the parent cgroup of any machine where the
  //   probe succeeded. Only undo what we added -- controllers the user already
  //   had enabled must not be touched.
  const bool had_pids = HasController(parent, "pids");
  const bool had_memory = HasController(parent, "memory");
  const auto restore = [&]() {
    std::string ignored;
    if (!had_pids) WriteFile(parent + "/cgroup.subtree_control", "-pids", &ignored);
    if (!had_memory) WriteFile(parent + "/cgroup.subtree_control", "-memory", &ignored);
  };

  // First try enabling pids and memory. If the parent already enabled them,
  // writing again is harmless; if it gives EBUSY because of internal
  // processes, this path does not work here.
  if (!WriteFile(parent + "/cgroup.subtree_control", "+pids +memory", &err)) {
    *reason = "enable subtree_control: " + err;
    return false;  // nothing was enabled, so there is nothing to restore
  }

  if (::mkdir(child.c_str(), 0755) != 0 && errno != EEXIST) {
    *reason = "mkdir(" + child + "): " + ::strerror(errno);
    restore();
    return false;
  }

  // 4) Verify both limit files really exist, are writable, and read back
  //    matching what was written -- "it exists" is not "the controller was
  //    delegated". Use conservative values: a child cgroup's memory.max
  //    cannot exceed its parent's, and too large a probe value can fail
  //    spuriously.
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
  // Read back to confirm the limit really took effect (this also catches the
  // case where the write succeeded but the kernel did not accept it).
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

  // 5) The probe succeeded: restore subtree_control and clean up the
  //    temporary cgroup.
  restore();
  if (::rmdir(child.c_str()) != 0 && errno != ENOENT) {
    // A failed cleanup does not change the "usable" conclusion, but recording
    // it is more honest.
    *reason = "usable (probe cgroup cleanup: " + Err("rmdir") + ")";
  } else {
    *reason = "ok";
  }
  *path = child;
  return true;
}

Cgroup2Session::~Cgroup2Session() {
  if (active_ && !path_.empty()) {
    // rmdir gives EBUSY while processes remain in the child cgroup; at that
    // point the limits are moot anyway (the processes may already have
    // finished), and leaving an empty directory for systemd/the kernel to
    // reclaim beats erroring out.
    if (::rmdir(path_.c_str()) != 0 && errno != ENOENT && errno != EBUSY) {
      // Silent: a destructor is no place to throw.
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

  // Enable the parent's pids + memory controllers (idempotent: writing again
  // when already enabled is harmless).
  if (!WriteFile(parent + "/cgroup.subtree_control", "+pids +memory", err)) {
    return false;
  }

  const std::string child = parent + "/" + ChildName("session");
  if (::mkdir(child.c_str(), 0755) != 0 && errno != EEXIST) {
    *err = "mkdir(" + child + "): " + ::strerror(errno);
    return false;
  }

  // Only write non-zero limits; 0 means that entry is unlimited, so leave the
  // kernel default (max) alone.
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
