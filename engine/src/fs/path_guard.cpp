#include "fs/path_guard.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>

#include "platform/platform.hpp"

namespace hx {
namespace {

// Split off the parent directory and the final component. The separator test
// goes through platform -- on Windows both '/' and '\\' count, and recognizing
// only one would treat a host-supplied path like "dir/file" as a single
// filename.
void SplitLast(const std::string& p, std::string* parent, std::string* leaf) {
  size_t slash = std::string::npos;
  for (size_t i = p.size(); i > 0; --i) {
    if (platform::IsSeparator(p[i - 1])) {
      slash = i - 1;
      break;
    }
  }
  if (slash == std::string::npos) {
    *parent = ".";
    *leaf = p;
  } else {
    *parent = platform::Parent(p);
    *leaf = p.substr(slash + 1);
  }
}

}  // namespace

bool IsWithin(const std::string& path, const std::string& root) {
  // ★ Delegated to platform, because "is it under the root" is not the same
  //   question on the two platforms: POSIX can simply compare bytes, while
  //   NTFS is case-insensitive by default, so "C:\Repo" and "c:\repo" are the
  //   same directory and a byte comparison would judge them two different
  //   roots -- and that gap is exactly where an escape gets in.
  return platform::IsWithin(path, root);
}

ResolveResult ResolveInRoots(const std::string& input, const std::string& base,
                            const std::vector<std::string>& roots, bool must_exist) {
  ResolveResult r;
  if (input.empty()) {
    r.error = "empty path";
    return r;
  }

  const std::string joined = platform::Join(base, input);

  std::string resolved;
  if (platform::RealPath(joined, &resolved)) {
    // Resolved: symlinks / junctions / 8.3 short names have all been unwound
  } else {
    if (must_exist) {
      r.error = "cannot resolve: " + joined;
      return r;
    }
    // The final component is allowed not to exist: resolve the parent
    // directory and rejoin. That way symlinks on the parent are unwound too
    // and nothing escapes.
    std::string parent;
    std::string leaf;
    SplitLast(joined, &parent, &leaf);
    if (leaf.empty() || leaf == "." || leaf == "..") {
      r.error = "invalid trailing path component";
      return r;
    }
    std::string presolved;
    if (!platform::RealPath(parent, &presolved)) {
      r.error = "cannot resolve parent: " + parent;
      return r;
    }
    resolved = platform::Join(presolved, leaf);
  }

  for (const auto& root : roots) {
    if (IsWithin(resolved, root)) {
      r.ok = true;
      r.path = resolved;
      return r;
    }
  }
  r.error = "path escapes workspace roots: " + resolved;
  return r;
}

}  // namespace hx
