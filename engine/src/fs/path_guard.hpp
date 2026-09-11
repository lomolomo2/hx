// 路径守卫 —— 唯一允许把"模型给的字符串"变成真实路径的地方。
//
// 这是 ProbeForRead 的语义版本（harness-windows-kernel-analogy.md §2.2）：
// 内核检查"指针是否在用户空间"，我们检查"路径解析后是否仍在 workspace 内"。
// Landlock 已经会在内核层拦住越界访问，这一层的价值是：
//   ① 给宿主一个明确的 E_PATH_ESCAPE，而不是让工具拿到一个含义模糊的 EACCES
//   ② danger-full-access 模式下 Landlock 不生效，这层仍然在
#pragma once

#include <string>
#include <vector>

namespace hx {

struct ResolveResult {
  bool ok = false;
  std::string path;   // ok 时为 realpath 后的绝对路径
  std::string error;
};

// 把 input（可相对 base）解析为绝对路径，并要求它落在 roots 之一内。
//
// must_exist=false 时允许最后一段不存在（写新文件），此时校验其父目录。
ResolveResult ResolveInRoots(const std::string& input, const std::string& base,
                            const std::vector<std::string>& roots, bool must_exist);

// path 是否在 root 之内（要求是路径分量边界，"/a/bc" 不算在 "/a/b" 内）
bool IsWithin(const std::string& path, const std::string& root);

}  // namespace hx
