#ifndef _WIN32
#include "exec/env.hpp"

namespace hx {

std::vector<std::string> BuildMinimalEnv(const std::string& home, const std::string& tmpdir,
                                         const std::map<std::string, std::string>& overrides) {
  std::vector<std::string> env = {
      "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
      "LANG=C.UTF-8",
      "TERM=dumb",
  };
  if (!home.empty()) env.push_back("HOME=" + home);
  // 编译器、打包器等大量工具要写临时文件。不给 TMPDIR 它们就会去 /tmp，
  // 而 /tmp 不在授权范围内 —— 于是构建类任务会莫名其妙地失败。
  if (!tmpdir.empty()) env.push_back("TMPDIR=" + tmpdir);

  for (const auto& [k, v] : overrides) env.push_back(k + "=" + v);
  return env;
}

}  // namespace hx
#endif  // !_WIN32
