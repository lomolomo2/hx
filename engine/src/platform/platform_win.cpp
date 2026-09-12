#ifdef _WIN32
#include "platform/platform.hpp"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "platform/win_util.hpp"

namespace hx::platform {
namespace {

int64_t FiletimeToUnixMs(const FILETIME& ft) {
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  // FILETIME 是 1601-01-01 起的 100ns 单位
  constexpr int64_t kEpochDiff100ns = 116444736000000000LL;
  return (static_cast<int64_t>(u.QuadPart) - kEpochDiff100ns) / 10000;
}

/**
 * 去掉 GetFinalPathNameByHandleW 的扩展前缀。
 *
 * ★ 为什么不直接留着：扩展前缀路径绕过 Win32 的路径规范化，很多第三方工具
 *   （以及 CreateProcess 的 lpCurrentDirectory）对它反应不一。引擎内部统一
 *   用普通 DOS 路径，需要长路径时在调用点自己加前缀。
 */
std::wstring StripExtendedPrefix(std::wstring p) {
  if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
  if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
  return p;
}

bool GetAttrs(const std::string& p, WIN32_FILE_ATTRIBUTE_DATA* out) {
  return ::GetFileAttributesExW(win::Widen(p).c_str(), GetFileExInfoStandard, out) != 0;
}

}  // namespace

int64_t NowMs() {
  // QPC 是单调的。deadline 全靠这个，绝不能用挂钟（GetSystemTime）——
  // 用户改一次系统时间就会让所有超时错乱。
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    return f;
  }();
  if (freq.QuadPart == 0) return static_cast<int64_t>(::GetTickCount64());
  LARGE_INTEGER now{};
  ::QueryPerformanceCounter(&now);
  return static_cast<int64_t>((now.QuadPart * 1000) / freq.QuadPart);
}

int64_t Pid() { return static_cast<int64_t>(::GetCurrentProcessId()); }

bool RealPath(const std::string& in, std::string* out) {
  // ★ 这是 Windows 上的「解析后路径」。
  //   打开句柄再问内核要最终路径，等价于 POSIX realpath：
  //   符号链接、junction、mount point、8.3 短名、大小写全部被解开。
  //   path_guard 的逃逸防护依赖这一点 —— 只用 GetFullPathNameW 做字符串
  //   规范化的话，junction 指出 root 的情况会被整个漏掉。
  const std::wstring w = win::Widen(in);
  HANDLE h = ::CreateFileW(w.c_str(), 0,  // 只要元数据，不要访问权
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,  // 目录也要能打开
                           nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  std::wstring buf(MAX_PATH, L'\0');
  DWORD n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n >= buf.size()) {
    buf.assign(static_cast<size_t>(n) + 1, L'\0');
    n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()),
                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  }
  ::CloseHandle(h);
  if (n == 0) return false;
  buf.resize(n);
  *out = win::Narrow(StripExtendedPrefix(std::move(buf)));
  return true;
}

bool IsDirectory(const std::string& p) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  return GetAttrs(p, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsRegularFile(const std::string& p) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  return GetAttrs(p, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool Stat(const std::string& p, StatInfo* out) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetAttrs(p, &a)) {
    out->exists = false;
    return false;
  }
  out->exists = true;
  out->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  out->is_regular = !out->is_dir;
  ULARGE_INTEGER sz{};
  sz.LowPart = a.nFileSizeLow;
  sz.HighPart = a.nFileSizeHigh;
  out->size = static_cast<int64_t>(sz.QuadPart);
  // Windows 没有 POSIX 权限位。合成一个诚实的近似：只读属性 -> 0444，否则 0644。
  // 协议里这个字段只用于展示，不参与任何决策。
  const bool ro = (a.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  out->mode = out->is_dir ? (ro ? 0555 : 0755) : (ro ? 0444 : 0644);
  out->mtime_ms = FiletimeToUnixMs(a.ftLastWriteTime);
  return true;
}

bool MakeDirs(const std::string& path, std::string* err) {
  const std::string norm = Normalize(path);
  size_t i = 0;
  // 跳过根（"C:\" 或 UNC 的 \\server\share）—— 对根调 CreateDirectory 必然失败
  if (norm.size() >= 2 && norm[1] == ':') {
    i = (norm.size() >= 3 && IsSeparator(norm[2])) ? 3 : 2;
  } else if (norm.size() >= 2 && IsSeparator(norm[0]) && IsSeparator(norm[1])) {
    size_t seg = 2;
    int seen = 0;
    while (seg < norm.size() && seen < 2) {
      if (IsSeparator(norm[seg])) ++seen;
      ++seg;
    }
    i = seg;
  }
  while (i <= norm.size()) {
    const size_t slash = norm.find('\\', i);
    const std::string acc = (slash == std::string::npos) ? norm : norm.substr(0, slash);
    if (!acc.empty() && ::CreateDirectoryW(win::Widen(acc).c_str(), nullptr) == 0) {
      const DWORD e = ::GetLastError();
      if (e != ERROR_ALREADY_EXISTS) {
        *err = "CreateDirectory(" + acc + "): " + win::ErrorMessage(e);
        return false;
      }
    }
    if (slash == std::string::npos) break;
    i = slash + 1;
  }
  return true;
}

bool MakeTempDir(const std::string& prefix, std::string* out, std::string* err) {
  std::wstring base(MAX_PATH + 1, L'\0');
  const DWORD n = ::GetTempPathW(static_cast<DWORD>(base.size()), base.data());
  if (n == 0) {
    *err = win::LastError("GetTempPath");
    return false;
  }
  base.resize(n);
  // ★ 不用 GetTempFileName：它会先建一个**文件**，我们要的是目录。
  //   CreateDirectory 本身是原子的，撞名就换一个再试。
  static unsigned counter = 0;
  for (int attempt = 0; attempt < 64; ++attempt) {
    char suffix[96];
    ::snprintf(suffix, sizeof(suffix), "%s%lu-%u-%llx", prefix.c_str(),
               static_cast<unsigned long>(::GetCurrentProcessId()), ++counter,
               static_cast<unsigned long long>(::GetTickCount64()));
    const std::string cand = win::Narrow(base) + suffix;
    if (::CreateDirectoryW(win::Widen(cand).c_str(), nullptr) != 0) {
      // ★ 回解析一次。GetTempPath 常常给出带 8.3 短名的路径（例如 RUNNER~1），
      //   而 roots 的包含判断用的是解析后路径。这里不解析，后面必然对不上，
      //   表现为「明明授权了 tmpdir，工具写临时文件还是被拒」。
      if (!RealPath(cand, out)) *out = cand;
      return true;
    }
    if (::GetLastError() != ERROR_ALREADY_EXISTS) {
      *err = win::LastError("CreateDirectory(temp)");
      return false;
    }
  }
  *err = "MakeTempDir: exhausted attempts";
  return false;
}

bool RemoveDir(const std::string& path) {
  return ::RemoveDirectoryW(win::Widen(path).c_str()) != 0;
}

bool GetEnv(const char* name, std::string* out) {
  const std::wstring wname = win::Widen(name);
  const DWORD n = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (n == 0) return false;
  std::wstring buf(n, L'\0');
  const DWORD got = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
  if (got == 0 || got >= n) return false;
  buf.resize(got);
  out->assign(win::Narrow(buf));
  return true;
}

std::string HomeDir() {
  std::string v;
  if (GetEnv("USERPROFILE", &v) && !v.empty()) return v;
  std::string drive, p;
  if (GetEnv("HOMEDRIVE", &drive) && GetEnv("HOMEPATH", &p)) return drive + p;
  if (GetEnv("HOME", &v)) return v;
  return ".";
}

bool ListDir(const std::string& dir, std::vector<std::string>* names) {
  WIN32_FIND_DATAW fd{};
  const std::string pattern = Join(dir, "*");
  HANDLE h = ::FindFirstFileExW(win::Widen(pattern).c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    const std::wstring w(fd.cFileName);
    if (w == L"." || w == L"..") continue;
    names->push_back(win::Narrow(w));
  } while (::FindNextFileW(h, &fd) != 0);
  ::FindClose(h);
  return true;
}

bool MatchComponent(const std::string& pattern, const std::string& name) {
  // ★ 不用 PathMatchSpecW：它带着一堆 MS-DOS 遗留特例（"*.*" 会匹配无扩展名的
  //   文件、末尾的点被忽略），同一个 glob 在两个平台上会给出不同结果。
  //   自己写一个与 fnmatch(FNM_PATHNAME) 语义一致的，只有大小写敏感性随平台。
  const std::string p = FoldCase(pattern);
  const std::string s = FoldCase(name);
  // 迭代式回溯：遇到 '*' 记住分叉点，失配就回去让它多吃一个字符。
  // 递归版在 "a*a*a*..." 这类模式上会指数爆炸。
  size_t pi = 0, si = 0;
  size_t star = std::string::npos, match = 0;
  while (si < s.size()) {
    bool advanced = false;
    if (pi < p.size() && p[pi] == '[') {
      // 字符类：[abc] / [!a-z] / []abc]
      size_t close = pi + 1;
      bool neg = false;
      if (close < p.size() && (p[close] == '!' || p[close] == '^')) {
        neg = true;
        ++close;
      }
      if (close < p.size() && p[close] == ']') ++close;  // 首位的 ] 是字面量
      while (close < p.size() && p[close] != ']') ++close;
      if (close < p.size()) {
        bool hit = false;
        for (size_t k = pi + 1 + (neg ? 1u : 0u); k < close; ++k) {
          if (k + 2 < close && p[k + 1] == '-') {
            if (s[si] >= p[k] && s[si] <= p[k + 2]) hit = true;
            k += 2;
          } else if (p[k] == s[si]) {
            hit = true;
          }
        }
        if (hit != neg && !IsSeparator(s[si])) {
          pi = close + 1;
          ++si;
          advanced = true;
        }
      } else if (p[pi] == s[si]) {  // 没有闭合的 ']'，'[' 当字面量
        ++pi;
        ++si;
        advanced = true;
      }
    } else if (pi < p.size() && p[pi] == '?') {
      if (!IsSeparator(s[si])) {  // FNM_PATHNAME：'?' 不跨分隔符
        ++pi;
        ++si;
        advanced = true;
      }
    } else if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      match = si;
      continue;
    } else if (pi < p.size() && p[pi] == s[si]) {
      ++pi;
      ++si;
      advanced = true;
    }
    if (advanced) continue;
    // 失配：回到上一个 '*'，让它多吃一个字符（但 '*' 同样不跨分隔符）
    if (star != std::string::npos && match < s.size() && !IsSeparator(s[match])) {
      pi = star + 1;
      si = ++match;
      continue;
    }
    return false;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

char PreferredSeparator() { return '\\'; }

bool IsSeparator(char c) { return c == '\\' || c == '/'; }

bool IsAbsolute(const std::string& p) {
  if (p.size() >= 2 && IsSeparator(p[0]) && IsSeparator(p[1])) return true;  // UNC
  return p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) != 0 && p[1] == ':' &&
         IsSeparator(p[2]);
}

std::string Join(const std::string& base, const std::string& rel) {
  if (IsAbsolute(rel)) return rel;
  if (base.empty()) return rel;
  if (IsSeparator(base.back())) return base + rel;
  return base + "\\" + rel;
}

std::string Normalize(const std::string& p) {
  std::string out = p;
  for (char& c : out) {
    if (c == '/') c = '\\';
  }
  return out;
}

std::string Parent(const std::string& p) {
  const std::string n = Normalize(p);
  const size_t slash = n.find_last_of('\\');
  if (slash == std::string::npos) return ".";
  // "C:\x" 的父是 "C:\"，不是 "C:"（后者是「当前目录」，语义完全不同）
  if (slash == 2 && n.size() >= 3 && n[1] == ':') return n.substr(0, 3);
  if (slash == 0) return "\\";
  return n.substr(0, slash);
}

std::string FoldCase(const std::string& p) {
  // ★ NTFS 默认大小写不敏感。roots 的包含判断若按字节比，
  //   "C:\Repo" 与 "c:\repo" 会被判成两个不同的 root —— 逃逸就从这里进来。
  std::string out = Normalize(p);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool IsWithin(const std::string& path, const std::string& root) {
  const std::string p = FoldCase(path);
  const std::string r = FoldCase(root);
  if (p.size() < r.size()) return false;
  if (p.compare(0, r.size(), r) != 0) return false;
  if (p.size() == r.size()) return true;
  if (IsSeparator(r.back())) return true;  // root 是 "C:\" 这种带尾分隔符的
  return IsSeparator(p[r.size()]);
}

}  // namespace hx::platform
#endif  // _WIN32
