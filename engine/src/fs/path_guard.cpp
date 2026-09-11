#include "fs/path_guard.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>

namespace hx {
namespace {

std::string JoinPath(const std::string& base, const std::string& rel) {
  if (rel.empty()) return base;
  if (rel.front() == '/') return rel;
  if (base.empty()) return rel;
  if (base.back() == '/') return base + rel;
  return base + "/" + rel;
}

// 拆出父目录与最后一段
void SplitLast(const std::string& p, std::string* parent, std::string* leaf) {
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos) {
    *parent = ".";
    *leaf = p;
  } else if (slash == 0) {
    *parent = "/";
    *leaf = p.substr(1);
  } else {
    *parent = p.substr(0, slash);
    *leaf = p.substr(slash + 1);
  }
}

}  // namespace

bool IsWithin(const std::string& path, const std::string& root) {
  if (path == root) return true;
  if (path.size() <= root.size()) return false;
  if (path.compare(0, root.size(), root) != 0) return false;
  // 必须落在路径分量边界上
  return root == "/" || path[root.size()] == '/';
}

ResolveResult ResolveInRoots(const std::string& input, const std::string& base,
                            const std::vector<std::string>& roots, bool must_exist) {
  ResolveResult r;
  if (input.empty()) {
    r.error = "empty path";
    return r;
  }

  const std::string joined = JoinPath(base, input);

  std::string resolved;
  char* real = ::realpath(joined.c_str(), nullptr);
  if (real != nullptr) {
    resolved.assign(real);
    ::free(real);
  } else {
    if (must_exist) {
      r.error = std::string("cannot resolve: ") + ::strerror(errno);
      return r;
    }
    // 允许最后一段不存在：解析父目录，再拼回来。
    // 这样符号链接父目录同样会被解开，逃逸不了。
    std::string parent;
    std::string leaf;
    SplitLast(joined, &parent, &leaf);
    if (leaf.empty() || leaf == "." || leaf == "..") {
      r.error = "invalid trailing path component";
      return r;
    }
    char* preal = ::realpath(parent.c_str(), nullptr);
    if (preal == nullptr) {
      r.error = std::string("cannot resolve parent: ") + ::strerror(errno);
      return r;
    }
    resolved.assign(preal);
    ::free(preal);
    if (resolved.back() != '/') resolved.push_back('/');
    resolved.append(leaf);
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
