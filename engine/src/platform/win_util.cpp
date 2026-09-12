#ifdef _WIN32
#include "platform/win_util.hpp"

#include <windows.h>

namespace hx::win {

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& utf16) {
  if (utf16.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                      nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), n,
                        nullptr, nullptr);
  return out;
}

std::string ErrorMessage(unsigned long code) {
  LPWSTR buf = nullptr;
  const DWORD n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buf), 0,
      nullptr);
  if (n == 0 || buf == nullptr) return "error " + std::to_string(code);
  std::wstring w(buf, n);
  ::LocalFree(buf);
  while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
  return Narrow(w) + " (" + std::to_string(code) + ")";
}

std::string LastError(const char* what) {
  return std::string(what) + ": " + ErrorMessage(::GetLastError());
}

}  // namespace hx::win
#endif  // _WIN32
