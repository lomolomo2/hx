#include "fs/path_guard.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>

#include "platform/platform.hpp"

namespace hx {
namespace {

// 拆出父目录与最后一段。分隔符判断走 platform —— Windows 上 '/' 和 '\\' 都算，
// 只认一种的话 "dir/file" 这种宿主传来的路径会被当成单个文件名。
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
  // ★ 委托给 platform，因为「在不在 root 之下」在两个平台上不是同一个问题：
  //   POSIX 按字节比较即可；Windows 的 NTFS 默认大小写不敏感，
  //   "C:\Repo" 与 "c:\repo" 是同一个目录，按字节比会判成两个不同的 root ——
  //   逃逸就从这个缝里进来。
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
    // 解析成功：符号链接 / junction / 8.3 短名都已经被解开
  } else {
    if (must_exist) {
      r.error = "cannot resolve: " + joined;
      return r;
    }
    // 允许最后一段不存在：解析父目录，再拼回来。
    // 这样父目录上的符号链接同样会被解开，逃逸不了。
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
