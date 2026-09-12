// Small Windows-only helpers: UTF-8 <-> UTF-16, error code to text.
//
// ★ This is the only place in the system that converts encodings.
//   The protocol (hxp/0) is UTF-8 JSON; the Win32 "wide" APIs are UTF-16.
//   Anywhere the two mix is a breeding ground for bugs -- a single non-ASCII
//   character in a path is enough to make an ANSI-flavoured API silently
//   operate on the wrong file. So the engine is UTF-8 throughout, converts to
//   UTF-16 only at the moment it calls Win32, and only ever calls the W APIs.
#pragma once
#ifdef _WIN32

#include <string>

namespace hx::win {

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& utf16);

/** Wrapper over FormatMessage: readable text for a GetLastError() code
 *  (trailing newlines stripped). */
std::string ErrorMessage(unsigned long code);

/** Equivalent to std::string(what) + ": " + ErrorMessage(GetLastError()). */
std::string LastError(const char* what);

}  // namespace hx::win

#endif  // _WIN32
