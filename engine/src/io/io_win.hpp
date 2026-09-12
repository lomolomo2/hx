// The Windows-only entry points io_win.cpp exposes.
//
// Only spawn_win.cpp and pty_win.cpp need this: they have to get at the raw
// HANDLE for "the end that goes to the child" in order to fill in STARTUPINFO.
// Nobody else should ever touch a HANDLE.
#pragma once
#ifdef _WIN32

#include <windows.h>

#include <string>

#include "io/io.hpp"

namespace hx::io::win {

/**
 * Create a pipe pair: the parent end overlapped and already attached to the
 * IOCP, the child end synchronous and inheritable.
 *
 * ★ Why not CreatePipe: it produces an anonymous pipe, which does not support
 *   overlapped I/O, so the parent end could only block on read -- and a
 *   single-threaded engine freezes the moment it does. The standard approach
 *   is to build the pair yourself from a uniquely named pipe: the parent end
 *   gets FILE_FLAG_OVERLAPPED, the child end a synchronous handle (the child
 *   is an ordinary program and assumes its stdio is synchronous).
 *
 * parent_reads = true  -> parent reads, child writes (the child's stdout/stderr)
 * parent_reads = false -> parent writes, child reads (the child's stdin)
 */
bool CreatePipePair(bool parent_reads, Fd* parent, HANDLE* child_end, std::string* err);

/**
 * Bring an already-existing overlapped HANDLE into the stream table (used for
 * the ConPTY master end). for_read / for_write select which sides to attach;
 * pass true for both when the same HANDLE serves both directions.
 */
Fd AdoptHandle(HANDLE h, bool for_read, bool for_write, std::string* err);

/** For diagnostics; the normal path should never need it. */
HANDLE RawHandle(Fd fd);

}  // namespace hx::io::win

#endif  // _WIN32
