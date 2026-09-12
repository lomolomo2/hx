#ifdef _WIN32
// The file-I/O half of platform_win.cpp. The split is purely because the
// path/time group and the file group share no state; together they would be
// a 700-line grab bag.
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
  // The temp file must sit in the target's own directory: MoveFileEx replaces
  // atomically only within one volume. Put it in %TEMP% and it degrades to
  // "copy + delete", which leaves half a file behind on a crash partway.
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
  if (::FlushFileBuffers(h) == 0) {  // the counterpart of fsync
    *err = win::LastError("FlushFileBuffers");
    ::CloseHandle(h);
    ::DeleteFileW(win::Widen(tmp_path).c_str());
    return false;
  }
  ::CloseHandle(h);

  // ★ MOVEFILE_REPLACE_EXISTING is the counterpart of rename(2);
  //   MOVEFILE_WRITE_THROUGH makes the replacement itself durable.
  //   Without REPLACE_EXISTING, MoveFile simply fails when the target already
  //   exists -- and apply_patch is overwhelmingly editing files that do.
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
  // ★ Must go through _wfopen. The narrow version interprets the path in the
  //   current ANSI code page, so a single non-ASCII or emoji directory name in
  //   the repo makes fs.read report "file does not exist" while the path looks
  //   perfectly fine in the protocol -- extremely hard to track down.
  std::FILE* f = nullptr;
  const std::wstring wmode = win::Widen(mode);
  if (::_wfopen_s(&f, win::Widen(path).c_str(), wmode.c_str()) != 0) return nullptr;
  return f;
}

AppendFile::~AppendFile() { Close(); }

bool AppendFile::Open(const std::string& path, std::string* err) {
  Close();
  // ★ FILE_APPEND_DATA only -- **not** FILE_WRITE_DATA.
  //   This is exactly the counterpart of O_APPEND: the kernel guarantees
  //   "seek to end + write" is one operation. Add FILE_WRITE_DATA and it
  //   degrades to an ordinary write, which can overwrite existing content
  //   under concurrency or after a crash.
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
  // One appending WriteFile: a process killed partway leaves no half line.
  // A short write must be reported as a failure -- the rollout is this
  // system's source of truth.
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
