#ifdef _WIN32
#include "exec/env.hpp"

#include <windows.h>

#include <vector>

#include "platform/platform.hpp"
#include "platform/win_util.hpp"

namespace hx {
namespace {

/**
 * 沙箱里的 PATH。
 *
 * ★ 这一条直接决定了沙箱好不好用，值得写清楚取舍。
 *
 *   Linux 侧写死 "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"。
 *   那能成立，是因为 FHS 保证工具链就在那几个目录里。Windows 没有 FHS ——
 *   node / git / python 装在 Program Files、ProgramData 甚至随便哪里，
 *   全靠 PATH 指路。写死一个列表等于让沙箱里什么都跑不了。
 *
 *   所以这里的做法是：**继承当前 PATH，但剔掉用户 profile 下的条目**。
 *   这恰好对上 Landlock 那一侧的语义 —— README 的「已知的坑」第一条说
 *   nvm / rustup / conda 装在 $HOME 的工具链在沙箱里跑不了，因为 $HOME
 *   默认不可读。Windows 上 AppContainer 同样读不到用户 profile，
 *   所以把那些条目留在 PATH 里只会制造更难查的症状：
 *   命令**找得到**却启动失败（ACCESS_DENIED），而不是干脆利落的 not found。
 *   剔掉它们，失败模式就和 Linux 一致了，而且解法也一致：显式加进 roots。
 */
std::string SandboxPath() {
  std::string raw;
  if (!platform::GetEnv("PATH", &raw)) raw.clear();
  const std::string home = platform::FoldCase(platform::HomeDir());

  std::vector<std::string> keep;
  size_t start = 0;
  while (start <= raw.size()) {
    const size_t semi = raw.find(';', start);
    const std::string entry =
        raw.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
    if (!entry.empty() && !platform::IsWithin(entry, home)) keep.push_back(entry);
    if (semi == std::string::npos) break;
    start = semi + 1;
  }

  // ★ 系统目录必须排在**最前面**，不是「在就行」。
  //
  //   继承来的 PATH 上往往躺着 MSYS / Git-Bash / chocolatey 的 bin，
  //   里面有一堆和系统命令同名的 shim。它们排在 System32 前面时，
  //   沙箱里跑的就不是模型以为的那个程序 —— 这比找不到命令更危险，
  //   因为它会**成功**，只是行为不对。
  //   （早先这里只检查"是否已存在"，于是 System32 因为出现在 PATH 靠后
  //     位置而不被前置，实测 "cmd" 解析到了 msys 的同名脚本。）
  std::vector<std::string> head;
  std::string sysroot;
  if (platform::GetEnv("SystemRoot", &sysroot) && !sysroot.empty()) {
    const std::string sys32 = platform::Join(sysroot, "System32");
    head = {sys32, sysroot, platform::Join(sys32, "Wbem"),
            platform::Join(platform::Join(sys32, "WindowsPowerShell"), "v1.0"),
            platform::Join(sys32, "OpenSSH")};
  }

  std::string out;
  auto emit = [&out](const std::string& e) {
    if (!out.empty()) out.push_back(';');
    out += e;
  };
  for (const auto& h : head) emit(h);
  for (const auto& k : keep) {
    // 去重：前置过的系统目录不再重复出现（尾部斜杠的写法也算同一个）
    std::string folded = platform::FoldCase(k);
    while (!folded.empty() && platform::IsSeparator(folded.back())) folded.pop_back();
    bool dup = false;
    for (const auto& h : head) {
      if (folded == platform::FoldCase(h)) {
        dup = true;
        break;
      }
    }
    if (!dup) emit(k);
  }
  return out;
}

/** 从当前环境原样带过去的变量。没有它们，大量 Win32 API 会以奇怪的方式失败。 */
void CopyThrough(const char* name, std::vector<std::string>* env) {
  std::string v;
  if (platform::GetEnv(name, &v)) env->push_back(std::string(name) + "=" + v);
}

}  // namespace

std::vector<std::string> BuildMinimalEnv(const std::string& home, const std::string& tmpdir,
                                         const std::map<std::string, std::string>& overrides) {
  std::vector<std::string> env;
  env.push_back("PATH=" + SandboxPath());
  env.push_back("PATHEXT=.COM;.EXE;.BAT;.CMD");
  // Linux 侧设 TERM=dumb，是为了让工具别输出 ANSI 控制序列污染上下文。
  // Windows 上对应的是这一对：关掉 .NET / PowerShell 的彩色输出。
  env.push_back("TERM=dumb");
  env.push_back("NO_COLOR=1");

  // ★ 这几个不是可选的。
  //   SystemRoot 少了，winsock、加密 API、甚至 cmd.exe 自己都会崩或行为异常；
  //   COMSPEC 少了，任何 system() / 批处理调用都会失败。
  //   Linux 侧不需要对应物，因为那边没有「系统目录要靠环境变量找」这回事。
  CopyThrough("SystemRoot", &env);
  CopyThrough("SystemDrive", &env);
  CopyThrough("windir", &env);
  CopyThrough("COMSPEC", &env);
  CopyThrough("NUMBER_OF_PROCESSORS", &env);
  CopyThrough("PROCESSOR_ARCHITECTURE", &env);

  // ★ LOCALAPPDATA 是 AppContainer 进程的**硬性前提**，不是便利。
  //
  //   AppContainer 的 profile 落在 %LOCALAPPDATA%\Packages\<name>\ 下，
  //   CreateProcess 在建容器时要从这个变量解析出那个目录。少了它，
  //   CreateProcess 直接失败，报的还是极具误导性的
  //   ERROR_ENVVAR_NOT_FOUND(203)「找不到输入的环境选项」——
  //   错误码完全没提 AppContainer，实测是靠逐个变量二分才定位到的。
  //   （只有它，APPDATA / USERPROFILE / ProgramData 之类都不需要。）
  //
  //   把它指向真实用户目录**不会**削弱隔离：变量存在不等于路径可读。
  //   AppContainer 在 %LOCALAPPDATA% 下只能进自己那个 package 子目录，
  //   其余部分照样 ACCESS_DENIED —— 和 Linux 侧「HOME 有值但 Landlock
  //   不放行」是同一回事。
  CopyThrough("LOCALAPPDATA", &env);

  // 与 Linux 侧一致：HOME 指向 workspace 根，而不是真实用户目录。
  // 沙箱本来也读不到真实 profile（AppContainer 不授予），指过去只会让
  // 工具去写一个必然失败的路径。
  if (!home.empty()) {
    env.push_back("HOME=" + home);
    env.push_back("USERPROFILE=" + home);
  }

  // 编译器、打包器等大量工具要写临时文件。Windows 上认的是 TEMP/TMP，
  // 不是 TMPDIR —— 名字给错，工具会退回 C:\Windows\Temp（沙箱无权），
  // 于是构建类任务会莫名其妙地失败。
  if (!tmpdir.empty()) {
    env.push_back("TEMP=" + tmpdir);
    env.push_back("TMP=" + tmpdir);
    env.push_back("TMPDIR=" + tmpdir);  // 跨平台工具（node、python）也认这个
  }

  for (const auto& [k, v] : overrides) env.push_back(k + "=" + v);
  return env;
}

}  // namespace hx
#endif  // _WIN32
