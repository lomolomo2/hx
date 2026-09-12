// The single place a child process's environment is constructed.
//
// ★ The test path (--sandbox-exec) and the production path (exec.start) must
//   use the same function: build one each and the tests pass in an environment
//   that does not exist in a real session. Been bitten once already:
//   --sandbox-exec did not set TMPDIR, so gcc reported "Cannot create
//   temporary file in /tmp/" while everything was fine in a real session.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace hx {

// Does not inherit the host's environ: environment variables are the easiest
// channel through which credentials leak.
std::vector<std::string> BuildMinimalEnv(const std::string& home, const std::string& tmpdir,
                                         const std::map<std::string, std::string>& overrides);

}  // namespace hx
