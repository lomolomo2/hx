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
  // FILETIME counts 100ns units since 1601-01-01
  constexpr int64_t kEpochDiff100ns = 116444736000000000LL;
  return (static_cast<int64_t>(u.QuadPart) - kEpochDiff100ns) / 10000;
}

/**
 * Strip the extended prefix that GetFinalPathNameByHandleW produces.
 *
 * ★ Why not just keep it: extended-prefix paths bypass Win32 path
 *   normalization, and plenty of third-party tools (as well as
 *   CreateProcess's lpCurrentDirectory) react to them inconsistently. The
 *   engine uses ordinary DOS paths internally and adds the prefix at the call
 *   site when a long path is actually needed.
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
  // QPC is monotonic. Every deadline rests on it; never the wall clock
  // (GetSystemTime) -- one change of the system time throws off all timeouts.
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
  // ★ This is the "resolved path" on Windows.
  //   Open a handle and ask the kernel for the final path -- the equivalent of
  //   POSIX realpath: symlinks, junctions, mount points, 8.3 short names and
  //   casing are all resolved. path_guard's escape protection depends on it;
  //   doing only string normalization with GetFullPathNameW would miss a
  //   junction pointing out of the root entirely.
  const std::wstring w = win::Widen(in);
  HANDLE h = ::CreateFileW(w.c_str(), 0,  // metadata only, no access rights
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,  // directories too
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
  // Windows has no POSIX permission bits. Synthesize an honest approximation:
  // the read-only attribute -> 0444, otherwise 0644. In the protocol this
  // field is display-only and feeds into no decision.
  const bool ro = (a.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  out->mode = out->is_dir ? (ro ? 0555 : 0755) : (ro ? 0444 : 0644);
  out->mtime_ms = FiletimeToUnixMs(a.ftLastWriteTime);
  return true;
}

bool MakeDirs(const std::string& path, std::string* err) {
  const std::string norm = Normalize(path);
  size_t i = 0;
  // Skip the root ("C:\", or \\server\share for UNC) -- calling
  // CreateDirectory on a root always fails
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
  // ★ Not GetTempFileName: it creates a **file** first, and we want a
  //   directory. CreateDirectory is itself atomic, so on a name collision we
  //   just pick another and retry.
  static unsigned counter = 0;
  for (int attempt = 0; attempt < 64; ++attempt) {
    char suffix[96];
    ::snprintf(suffix, sizeof(suffix), "%s%lu-%u-%llx", prefix.c_str(),
               static_cast<unsigned long>(::GetCurrentProcessId()), ++counter,
               static_cast<unsigned long long>(::GetTickCount64()));
    const std::string cand = win::Narrow(base) + suffix;
    if (::CreateDirectoryW(win::Widen(cand).c_str(), nullptr) != 0) {
      // ★ Resolve it once more. GetTempPath frequently hands back a path
      //   containing 8.3 short names (e.g. RUNNER~1), while root containment
      //   checks operate on resolved paths. Skip this and the two can never
      //   match, which shows up as "the tmpdir is clearly authorized, yet a
      //   tool writing a temp file still gets denied".
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
  // ★ Not PathMatchSpecW: it carries a pile of MS-DOS legacy special cases
  //   ("*.*" matches files with no extension, trailing dots are ignored), so
  //   the same glob would give different results on the two platforms.
  //   Hand-roll one matching fnmatch(FNM_PATHNAME) semantics instead; only
  //   case sensitivity varies by platform.
  const std::string p = FoldCase(pattern);
  const std::string s = FoldCase(name);
  // Iterative backtracking: on '*' remember the branch point, and on a
  // mismatch go back and let it consume one more character.
  // A recursive version blows up exponentially on patterns like "a*a*a*...".
  size_t pi = 0, si = 0;
  size_t star = std::string::npos, match = 0;
  while (si < s.size()) {
    bool advanced = false;
    if (pi < p.size() && p[pi] == '[') {
      // Character class: [abc] / [!a-z] / []abc]
      size_t close = pi + 1;
      bool neg = false;
      if (close < p.size() && (p[close] == '!' || p[close] == '^')) {
        neg = true;
        ++close;
      }
      if (close < p.size() && p[close] == ']') ++close;  // a leading ] is literal
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
      } else if (p[pi] == s[si]) {  // no closing ']', so '[' is literal
        ++pi;
        ++si;
        advanced = true;
      }
    } else if (pi < p.size() && p[pi] == '?') {
      if (!IsSeparator(s[si])) {  // FNM_PATHNAME: '?' does not cross a separator
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
    // Mismatch: go back to the last '*' and let it eat one more character
    // (but '*' does not cross a separator either)
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
  // The parent of "C:\x" is "C:\", not "C:" (the latter means "the current
  // directory on drive C" -- entirely different semantics)
  if (slash == 2 && n.size() >= 3 && n[1] == ':') return n.substr(0, 3);
  if (slash == 0) return "\\";
  return n.substr(0, slash);
}

std::string FoldCase(const std::string& p) {
  // ★ NTFS is case-insensitive by default. If root containment compared
  //   bytes, "C:\Repo" and "c:\repo" would be judged two different roots --
  //   and that is exactly where an escape gets in.
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
  if (IsSeparator(r.back())) return true;  // root already ends in a separator, e.g. "C:\"
  return IsSeparator(p[r.size()]);
}

}  // namespace hx::platform
#endif  // _WIN32
