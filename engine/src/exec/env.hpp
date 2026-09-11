// 子进程环境的唯一构造点。
//
// ★ 测试路径（--sandbox-exec）和生产路径（exec.start）必须用同一个函数：
//   两者若各建一份，测试就会在一个真实会话里不存在的环境下通过。
//   踩过一次：--sandbox-exec 没设 TMPDIR，于是 gcc 报
//   "Cannot create temporary file in /tmp/"，而真实会话里一切正常。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace hx {

// 不继承宿主 environ：环境变量是最容易漏出凭据的通道。
std::vector<std::string> BuildMinimalEnv(const std::string& home, const std::string& tmpdir,
                                         const std::map<std::string, std::string>& overrides);

}  // namespace hx
