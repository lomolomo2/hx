// The path guard -- the only place allowed to turn "a string the model
// supplied" into a real path.
//
// This is the semantic version of ProbeForRead
// (harness-windows-kernel-analogy.md section 2.2): the kernel checks "is this
// pointer in user space", and we check "after resolution, is this path still
// inside the workspace". Landlock already blocks out-of-bounds access at the
// kernel level, so what this layer adds is:
//   1. a definite E_PATH_ESCAPE for the host, rather than leaving a tool with
//      an ambiguous EACCES
//   2. under danger-full-access Landlock does not apply, and this layer is
//      still there
#pragma once

#include <string>
#include <vector>

namespace hx {

struct ResolveResult {
  bool ok = false;
  std::string path;   // when ok, the absolute path after realpath
  std::string error;
};

// Resolve input (which may be relative to base) to an absolute path, and
// require that it lands inside one of roots.
//
// With must_exist=false the final component may not exist (writing a new
// file), in which case its parent directory is validated instead.
ResolveResult ResolveInRoots(const std::string& input, const std::string& base,
                            const std::vector<std::string>& roots, bool must_exist);

// Is path inside root? (On a path-component boundary, so "/a/bc" does not
// count as inside "/a/b".)
bool IsWithin(const std::string& path, const std::string& root);

}  // namespace hx
