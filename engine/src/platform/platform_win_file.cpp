#ifdef _WIN32
// platform_win.cpp 的文件 I/O 那一半。拆成两个文件纯粹是因为
// 路径/时间那一组和文件那一组没有共享状态，放一起就是 700 行的大杂烩。
#include "platform/platform.hpp"

#include <windows.h>

#include <cstdio>
#include <ctime>
#include <vector>

#include "platform/win_util.hpp"

namespace hx::platform {

bool ReadWholeFile(const std::string& path, std::string* out, std::string* err) {
  HANDLE h = ::CreateFileW(win::Widen(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = win::LastError("open");
    return false;
  }
  out->clear();
  std::vector<char> buf(65536);
  for (;;) {
    DWORD got = 0;
    if (::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) == 0) {
      *err = win::LastError("read");
      ::CloseHandle(h);
      return false;
    }
    if (got == 0) break;
    out->append(buf.data(), got);
  }
  ::CloseHandle(h);
  return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& content, std::string* err) {
  const std::string dir = Parent(path);
  // 临时文件必须与目标同目录：MoveFileEx 的原子替换只在同一个卷上成立，
  // 放到 %TEMP% 去就退化成「复制 + 删除」，崩在中途会留下半个文件。
  static unsigned counter = 0;
  char suffix[64];
  ::snprintf(suffix, sizeof(suffix), ".hx-tmp-%lu-%u",
             static_cast<unsigned long>(::GetCurrentProcessId()), ++counter);
  const std::string tmp_path = Join(dir, suffix);

  HANDLE h = ::CreateFileW(win::Widen(tmp_path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = win::LastError("create temp");
    return false;
  }
  size_t off = 0;
  while (off < content.size()) {
    DWORD wrote = 0;
    const DWORD chunk = static_cast<DWORD>(
        content.size() - off > 0x10000000u ? 0x10000000u : content.size() - off);
    if (::WriteFile(h, content.data() + off, chunk, &wrote, nullptr) == 0) {
      *err = win::LastError("write");
      ::CloseHandle(h);
      ::DeleteFileW(win::Widen(tmp_path).c_str());
      return false;
    }
    off += wrote;
  }
  if (::FlushFileBuffers(h) == 0) {  // fsync 的对位
    *err = win::LastError("FlushFileBuffers");
    ::CloseHandle(h);
    ::DeleteFileW(win::Widen(tmp_path).c_str());
    return false;
  }
  ::CloseHandle(h);

  // ★ MOVEFILE_REPLACE_EXISTING 是 rename(2) 的对位；
  //   MOVEFILE_WRITE_THROUGH 让替换动作本身也落盘。
  //   少了 REPLACE_EXISTING，目标已存在时 MoveFile 直接失败 ——
  //   而 apply_patch 绝大多数情况都是在改已存在的文件。
  if (::MoveFileExW(win::Widen(tmp_path).c_str(), win::Widen(path).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    *err = win::LastError("MoveFileEx");
    ::DeleteFileW(win::Widen(tmp_path).c_str());
    return false;
  }
  return true;
}

bool Unlink(const std::string& path) {
  return ::DeleteFileW(win::Widen(path).c_str()) != 0;
}

std::FILE* FopenUtf8(const std::string& path, const char* mode) {
  // ★ 必须走 _wfopen。窄字符版按当前 ANSI 代码页解释路径，
  //   仓库里一个中文或 emoji 目录名就会让 fs.read 报「文件不存在」，
  //   而路径在协议里看起来完全正常 —— 极难查。
  std::FILE* f = nullptr;
  const std::wstring wmode = win::Widen(mode);
  if (::_wfopen_s(&f, win::Widen(path).c_str(), wmode.c_str()) != 0) return nullptr;
  return f;
}

AppendFile::~AppendFile() { Close(); }

bool AppendFile::Open(const std::string& path, std::string* err) {
  Close();
  // ★ 只要 FILE_APPEND_DATA、**不要** FILE_WRITE_DATA。
  //   这正是 O_APPEND 的对位：内核保证「定位到末尾 + 写」是一次操作。
  //   加上 FILE_WRITE_DATA 就退化成普通写，并发或崩溃时可能覆盖已有内容。
  HANDLE h = ::CreateFileW(win::Widen(path).c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = "open " + path + ": " + win::ErrorMessage(::GetLastError());
    return false;
  }
  h_ = reinterpret_cast<std::intptr_t>(h);
  return true;
}

bool AppendFile::WriteRecord(const std::string& s) {
  if (h_ == -1) return false;
  DWORD wrote = 0;
  // 一次 WriteFile 追加：进程中途被杀也不会留下半行。
  // 短写必须当失败上报 —— rollout 是这个系统的事实来源。
  if (::WriteFile(reinterpret_cast<HANDLE>(h_), s.data(), static_cast<DWORD>(s.size()), &wrote,
                  nullptr) == 0) {
    return false;
  }
  return wrote == s.size();
}

bool AppendFile::Sync() {
  return h_ != -1 && ::FlushFileBuffers(reinterpret_cast<HANDLE>(h_)) != 0;
}

void AppendFile::Close() {
  if (h_ == -1) return;
  ::FlushFileBuffers(reinterpret_cast<HANDLE>(h_));
  ::CloseHandle(reinterpret_cast<HANDLE>(h_));
  h_ = -1;
}

std::string LocalDatePath() {
  SYSTEMTIME st{};
  ::GetLocalTime(&st);
  char buf[32];
  ::snprintf(buf, sizeof(buf), "%04d/%02d/%02d", st.wYear, st.wMonth, st.wDay);
  return buf;
}

int64_t UnixNowMs() {
  FILETIME ft{};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  constexpr int64_t kEpochDiff100ns = 116444736000000000LL;
  return (static_cast<int64_t>(u.QuadPart) - kEpochDiff100ns) / 10000;
}

}  // namespace hx::platform
#endif  // _WIN32
