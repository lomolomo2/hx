// The platform seam for I/O and the event loop.
//
// engine.cpp's loop logic (timeouts, reaping, backpressure, dropped events) is
// OS-independent, but the original shape of it was nailed to epoll + fds. This
// layer factors out "which streams are ready to read or write", implemented
// separately on each side:
//
//   Linux   -- epoll; Fd really is a file descriptor and Read/Write are
//              ::read/::write
//   Windows -- IOCP + overlapped named pipes; Fd is an index into a stream table
//
// ★ POSIX non-blocking fds define the semantics (because that is what
//   engine.cpp was written against):
//     Read  returns >0 = bytes read, 0 = EOF, kWouldBlock = nothing for now,
//           kIoError = broken
//     Write returns >=0 = bytes accepted, kWouldBlock = cannot take a single byte
//   The Windows side is responsible for disguising the overlapped completion
//   model as that, rather than making engine.cpp carry two code paths -- two
//   paths would mean two loops to maintain.
#pragma once

#include <cstddef>
#include <string>

namespace hx::io {

/**
 * A stream handle.
 *
 * ★ Deliberately not called fd: on Windows it is not a file descriptor but an
 *   index into the stream table. A name containing "fd" would tempt someone
 *   into passing it straight to ::read -- which is wrong on Windows.
 */
using Fd = int;

inline constexpr Fd kInvalid = -1;

/** The two standard streams to the host. On Windows they take a separate
 *  implementation path; see the notes in io_win.cpp. */
inline constexpr Fd kStdin = 0;
inline constexpr Fd kStdout = 1;

/** Non-byte-count return values from Read/Write. */
inline constexpr long kWouldBlock = -1;
inline constexpr long kIoError = -2;

/** Must be called once before any I/O: takes over stdin/stdout and registers
 *  their Fds. */
bool InitStdio(std::string* err);

/** Read. Semantics as described at the top of this file. */
long Read(Fd fd, void* buf, size_t len);

/** Write. Semantics as at the top of this file; the return value may be less
 *  than len (a partial write) and the caller must hold on to the remainder. */
long Write(Fd fd, const void* buf, size_t len);

/** Close. Calling it on a cell's stdin write end sends EOF to the child. */
void Close(Fd fd);

/** One ready event returned by Wait. */
struct Event {
  Fd fd = kInvalid;
  bool readable = false;
  bool writable = false;
};

/**
 * The event loop's waiter.
 *
 * ★ Level-triggered semantics: while unread data remains, the next Wait must
 *   keep reporting readable. engine.cpp's OnCellFdReadable reads at most 8
 *   rounds before yielding (so a high-output child cannot starve timeouts and
 *   reaping), and it relies on being called back again after yielding.
 *   Edge-triggered would make that code silently drop data.
 */
class Reactor {
 public:
  Reactor();
  ~Reactor();
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  bool Open(std::string* err);

  /** Watch for readability. */
  bool AddRead(Fd fd);

  /** Turn writability interest on or off. Turn it on only when the outbound
   *  buffer stops draining, and off the moment it empties. */
  bool WatchWrite(Fd fd, bool on);

  /** Stop watching. Does not Close. */
  void Del(Fd fd);

  /** timeout_ms < 0 waits forever. Returns the number of ready events, or -1
   *  on error with err filled in. */
  int Wait(int timeout_ms, Event* out, int max, std::string* err);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace hx::io
