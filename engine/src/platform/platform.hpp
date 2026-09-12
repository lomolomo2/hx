// Platform primitives.
//
// The only reason this layer exists: the logic in engine.cpp / rollout.cpp /
// path_guard.cpp that is inherently OS-independent should not be nailed to
// POSIX by small things like realpath / mkdir / clock_gettime.
//
// ★ Draw the boundary precisely: anything carrying real semantics (sandbox,
//   process groups, pty) does NOT belong here -- each of those has its own
//   seam (sandbox/confine.hpp, exec/proc.hpp, exec/pty.hpp). This file holds
//   only calls that exist on both sides and merely spell differently. Let
//   semantics in and it degenerates into a util.h that everything gets
//   dumped into.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hx::platform {

/** Monotonic clock, milliseconds. Every deadline uses it -- never the wall
 *  clock, or changing the system time throws off all timeouts. */
int64_t NowMs();

/** This process's pid (logging and ping only; a real pid never appears in the
 *  protocol). */
int64_t Pid();

/** Resolve to a canonical absolute path; false if it does not exist.
 *  POSIX realpath / Windows GetFinalPathNameByHandle. */
bool RealPath(const std::string& in, std::string* out);

bool IsDirectory(const std::string& p);
bool IsRegularFile(const std::string& p);

struct StatInfo {
  bool exists = false;
  bool is_dir = false;
  bool is_regular = false;
  int64_t size = 0;
  int mode = 0;  // POSIX permission bits; a synthesized value on Windows
                 // (read-only = 0444)
  int64_t mtime_ms = 0;
};
bool Stat(const std::string& p, StatInfo* out);

/** Create a directory tree with mode 0700 (on Windows this relies on
 *  inherited ACLs; see the implementation comment for the semantics). */
bool MakeDirs(const std::string& path, std::string* err);

/** Create a temporary directory only this user can enter. `prefix` is just a
 *  readability prefix. */
bool MakeTempDir(const std::string& prefix, std::string* out, std::string* err);

/** Remove an empty directory. Fails when non-empty -- deliberately, so
 *  leftovers stay visible for diagnosis. */
bool RemoveDir(const std::string& path);

/** The user's home directory. Rollouts land under it by default. */
std::string HomeDir();

bool GetEnv(const char* name, std::string* out);

/** Enumerate directory entries. Returns false on failure (callers always skip
 *  it -- one unreadable directory must not fail an entire glob). */
bool ListDir(const std::string& dir, std::vector<std::string>* names);

/** Glob match for a single path component (the equivalent of POSIX
 *  fnmatch(FNM_PATHNAME)). */
bool MatchComponent(const std::string& pattern, const std::string& name);

// ---- Files ----

/** Read an entire file. */
bool ReadWholeFile(const std::string& path, std::string* out, std::string* err);

/**
 * Atomic replace: temp file in the same directory -> flush -> rename.
 * A crash partway through leaves either the old content or the new one,
 * never half a file.
 */
bool WriteFileAtomic(const std::string& path, const std::string& content, std::string* err);

bool Unlink(const std::string& path);

/** Open a FILE* by UTF-8 path (wide characters on Windows, or non-ASCII paths
 *  silently fail to open). */
std::FILE* FopenUtf8(const std::string& path, const char* mode);

/**
 * Append-only handle. The rollout's durability promise rests entirely on it:
 *   - one write() per record -> a hard kill never leaves half a line
 *   - power-loss durability comes from Sync()
 *
 * ★ The two platforms reach "atomic append" by different routes, but the
 *   guarantee is the same:
 *     POSIX   O_APPEND: seek and write are a single operation in the kernel
 *     Windows FILE_APPEND_DATA without FILE_WRITE_DATA: likewise guaranteed
 *                                                       by the kernel
 *   Both hold only for a single write that fits the pipe/sector buffer, so a
 *   short write must be reported as a failure rather than papered over -- the
 *   log is this system's source of truth; better to error than to leave half
 *   a line.
 */
class AppendFile {
 public:
  AppendFile() = default;
  ~AppendFile();
  AppendFile(const AppendFile&) = delete;
  AppendFile& operator=(const AppendFile&) = delete;

  bool Open(const std::string& path, std::string* err);
  bool WriteRecord(const std::string& s);
  bool Sync();
  void Close();
  bool valid() const { return h_ != -1; }

 private:
  std::intptr_t h_ = -1;  // POSIX: fd. Windows: HANDLE
};

/** Local date, shaped like "2026/09/11" (the rollout directory layout). */
std::string LocalDatePath();

/** Wall-clock milliseconds (for recorded timestamps; deadlines always use
 *  NowMs). */
int64_t UnixNowMs();

// ---- Path shape ----
//
// ★ A Windows path is not "a string separated by /": it has drive letters,
//   UNC forms, and is case-insensitive. path_guard is the single place in the
//   system that turns a string into a path, and it has to be able to ask
//   these questions -- otherwise root containment checks are simply wrong on
//   Windows.

/** The preferred separator ('/' or '\'). */
char PreferredSeparator();

/** Is this a separator? (On Windows both '/' and '\' count.) */
bool IsSeparator(char c);

/** Absolute path? Windows: "C:\x" or "\\server\share"; POSIX: starts with '/'. */
bool IsAbsolute(const std::string& p);

/** Append `rel` to `base` (returns `rel` unchanged when it is absolute). */
std::string Join(const std::string& base, const std::string& rel);

/** Normalize separators: all '\' on Windows, unchanged on POSIX. */
std::string Normalize(const std::string& p);

/** The parent directory. Returns the root when there is no parent. */
std::string Parent(const std::string& p);

/** The folded form used for path equality and prefix comparison (Windows is
 *  case-insensitive -> lowercased). */
std::string FoldCase(const std::string& p);

/** Is `path` equal to `root` or below it? Both must already be normalized
 *  absolute paths. */
bool IsWithin(const std::string& path, const std::string& root);

}  // namespace hx::platform
