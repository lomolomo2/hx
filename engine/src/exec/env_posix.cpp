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
  // Compilers, bundlers and plenty of other tools write temp files. Without
  // TMPDIR they go to /tmp, which is not in the authorized set -- so build
  // tasks fail for no visible reason.
  if (!tmpdir.empty()) env.push_back("TMPDIR=" + tmpdir);

  for (const auto& [k, v] : overrides) env.push_back(k + "=" + v);
  return env;
}

}  // namespace hx
#endif  // !_WIN32
