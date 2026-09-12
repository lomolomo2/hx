// Windows 专用小工具：UTF-8 ↔ UTF-16、错误码转文字。
//
// ★ 全系统只有一处做编码转换。
//   协议（hxp/0）是 UTF-8 的 JSON，Win32 的「宽字符」API 是 UTF-16。
//   两者混用的地方就是 bug 的温床 —— 路径里一个中文字符就能让
//   ANSI 版 API 静默拿到错的文件。所以引擎内部一律 UTF-8，
//   只在调 Win32 的那一刻转成 UTF-16，并且只调 W 系列 API。
#pragma once
#ifdef _WIN32

#include <string>

namespace hx::win {

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& utf16);

/** FormatMessage 的封装，拿 GetLastError() 的可读文字（已去掉尾部换行）。 */
std::string ErrorMessage(unsigned long code);

/** 等价于 std::string(what) + ": " + ErrorMessage(GetLastError())。 */
std::string LastError(const char* what);

}  // namespace hx::win

#endif  // _WIN32
