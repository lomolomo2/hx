#ifdef _WIN32
#include "io/io_win.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// ★ Note that hx::win and hx::io::win are two different namespaces.
//   Inside namespace hx::io, writing win::Foo resolves to the latter (the one
//   at the bottom of this file), so calls into win_util are always spelled out
//   in full as hx::win::.
#include "platform/win_util.hpp"

namespace hx::io {
namespace {

constexpr size_t kChunk = 64u * 1024u;

// ★ One IOCP for the whole process.
//   The engine has exactly one event loop to begin with (engine.cpp's comments
//   are explicit: single-threaded, lock-free), so this is not a per-Reactor
//   instance -- that would only force CreatePipePair to thread a Reactor&
//   everywhere, buying flexibility that is never used.
HANDLE g_iocp = nullptr;

/**
 * One stream.
 *
 * Windows overlapped I/O is a "completion" model; engine.cpp wants POSIX's
 * "readiness" model. The difference is stored here: we always keep one
 * ReadFile posted, completed data lands in rbuf, and the layer above reads
 * from rbuf -- so what it sees is a non-blocking fd.
 */
struct Stream {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool watched_read = false;
  bool watched_write = false;
  bool closed = false;

  // ---- read side ----
  OVERLAPPED ov_read{};
  std::vector<char> rbuf;
  size_t ravail = 0;     // valid bytes in rbuf
  size_t rconsumed = 0;  // how much the layer above has taken
  bool read_pending = false;
  bool eof = false;
  bool rerror = false;

  // ---- write side ----
  OVERLAPPED ov_write{};
  std::string wbuf;   // bytes accepted but not yet handed to the OS
  size_t wsent = 0;   // how much of wbuf has gone out
  bool write_pending = false;
  bool werror = false;

  bool ReadableNow() const {
    return (ravail > rconsumed) || eof || rerror;
  }
  bool WritableNow() const { return !write_pending; }
};

// The stream table. The index is the Fd; 0/1 are permanently reserved for
// stdin/stdout (which go through StdioBridge and have no Stream).
std::vector<Stream*> g_streams;

Stream* Get(Fd fd) {
  if (fd < 0 || static_cast<size_t>(fd) >= g_streams.size()) return nullptr;
  return g_streams[static_cast<size_t>(fd)];
}

Fd Install(Stream* s) {
  for (size_t i = 2; i < g_streams.size(); ++i) {
    if (g_streams[i] == nullptr) {
      g_streams[i] = s;
      return static_cast<Fd>(i);
    }
  }
  if (g_streams.size() < 2) g_streams.resize(2, nullptr);
  g_streams.push_back(s);
  return static_cast<Fd>(g_streams.size() - 1);
}

/**
 * A stream that is closed and has no I/O still in flight is only actually
 * freed here.
 *
 * See the note in Close(): completion packets come back carrying a pointer to
 * an OVERLAPPED that lives inside the Stream, so freeing it any earlier hands
 * the kernel memory to write into.
 */
void MaybeReap(Fd fd, Stream* s) {
  if (!s->closed || s->read_pending || s->write_pending) return;
  g_streams[static_cast<size_t>(fd)] = nullptr;
  delete s;
}

/** Post a read. Does nothing if one is already in flight. */
void PostRead(Fd fd, Stream* s) {
  if (s->read_pending || s->eof || s->rerror || s->closed) return;
  if (s->rconsumed < s->ravail) return;  // buffer not drained yet; do not overwrite
  s->ravail = 0;
  s->rconsumed = 0;
  if (s->rbuf.size() < kChunk) s->rbuf.resize(kChunk);
  ::ZeroMemory(&s->ov_read, sizeof(s->ov_read));
  s->read_pending = true;
  if (::ReadFile(s->h, s->rbuf.data(), static_cast<DWORD>(kChunk), nullptr, &s->ov_read) == 0) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_IO_PENDING) return;  // the normal path
    s->read_pending = false;
    if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_HANDLE_EOF) {
      s->eof = true;
    } else {
      s->rerror = true;
    }
  }
  // Even on synchronous success a completion packet is still queued (the
  // handle is associated with the IOCP), so Wait handles both uniformly
  (void)fd;
}

/** Post whatever part of wbuf has not gone out yet. */
void PostWrite(Fd fd, Stream* s) {
  if (s->write_pending || s->werror || s->closed) return;
  if (s->wsent >= s->wbuf.size()) {
    s->wbuf.clear();
    s->wsent = 0;
    return;
  }
  ::ZeroMemory(&s->ov_write, sizeof(s->ov_write));
  s->write_pending = true;
  const size_t remain = s->wbuf.size() - s->wsent;
  if (::WriteFile(s->h, s->wbuf.data() + s->wsent,
                  static_cast<DWORD>(remain > kChunk ? kChunk : remain), nullptr,
                  &s->ov_write) == 0) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_IO_PENDING) return;
    s->write_pending = false;
    s->werror = true;
  }
  (void)fd;
}

// ---------------------------------------------------------------- stdio
//
// ★ This is the one place in the whole Windows port where threads could not
//   be avoided, so it is worth spelling out why.
//
//   The host (Node) creates its stdio pipes through libuv, and the end handed
//   to the child process is a **synchronous** handle -- libuv does this
//   deliberately, because ordinary child processes assume their stdio is
//   synchronous. A synchronous handle cannot be associated with an IOCP
//   (CreateIoCompletionPort fails with ERROR_INVALID_PARAMETER), and ReadFile
//   on it blocks.
//
//   That leaves three options:
//     1. Poll with PeekNamedPipe -- no threads, but every request pays an
//        extra polling interval of latency for nothing
//     2. Change the transport (named pipes) -- requires changing the protocol;
//        proto/hxp-v0.md says stdio explicitly
//     3. Two byte-shuffling threads -- chosen
//
//   The cost must be recorded honestly: the README's "single-threaded,
//   lock-free, race-free" is no longer literally true on Windows. But **the
//   engine's state is still single-threaded**: these two threads only move
//   bytes and touch nothing in cells_/waits_/policy_; the handoff is the two
//   queues below. The surface exposed to races is compressed down to one
//   deque plus one mutex, rather than being scattered through the event loop.
struct StdioBridge {
  HANDLE in_h = INVALID_HANDLE_VALUE;
  HANDLE out_h = INVALID_HANDLE_VALUE;

  std::mutex mu;
  std::condition_variable cv_writer;   // tells the writer thread there is new data
  std::condition_variable cv_reader;   // tells the reader thread it may read on

  std::string in_buf;                  // reader thread -> main thread
  bool in_eof = false;
  std::string out_buf;                 // main thread -> writer thread
  bool out_broken = false;
  bool stopping = false;

  std::thread reader;
  std::thread writer;

  // Backpressure: do not buffer without bound when the host floods us
  static constexpr size_t kInCap = 8u * 1024u * 1024u;
  static constexpr size_t kOutCap = 16u * 1024u * 1024u;

  void Wake() {
    // Just wakes the event loop; no actual I/O completed. Key is kStdin and
    // lpOverlapped is null.
    ::PostQueuedCompletionStatus(g_iocp, 0, static_cast<ULONG_PTR>(kStdin), nullptr);
  }

  void ReaderLoop() {
    std::vector<char> buf(kChunk);
    for (;;) {
      DWORD got = 0;
      const BOOL ok = ::ReadFile(in_h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
      std::unique_lock<std::mutex> lk(mu);
      if (stopping) return;
      if (ok == 0 || got == 0) {
        in_eof = true;
        lk.unlock();
        Wake();
        return;
      }
      in_buf.append(buf.data(), got);
      lk.unlock();
      Wake();
      // Stop reading until the main thread has digested what it has, so a
      // chatty host cannot blow us up
      lk.lock();
      cv_reader.wait(lk, [this] { return stopping || in_buf.size() < kInCap; });
      if (stopping) return;
    }
  }

  void WriterLoop() {
    for (;;) {
      std::string chunk;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv_writer.wait(lk, [this] { return stopping || !out_buf.empty(); });
        if (stopping && out_buf.empty()) return;
        chunk.swap(out_buf);
      }
      size_t off = 0;
      bool broke = false;
      while (off < chunk.size()) {
        DWORD wrote = 0;
        if (::WriteFile(out_h, chunk.data() + off, static_cast<DWORD>(chunk.size() - off), &wrote,
                        nullptr) == 0 ||
            wrote == 0) {
          broke = true;
          break;
        }
        off += wrote;
      }
      {
        std::lock_guard<std::mutex> lk(mu);
        if (broke) out_broken = true;
      }
      Wake();  // drained -> the main thread may push more
      if (broke) return;
    }
  }
};

StdioBridge* g_stdio = nullptr;

}  // namespace

bool InitStdio(std::string* err) {
  if (g_streams.size() < 2) g_streams.resize(2, nullptr);
  if (g_stdio != nullptr) return true;
  g_stdio = new StdioBridge();
  g_stdio->in_h = ::GetStdHandle(STD_INPUT_HANDLE);
  g_stdio->out_h = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (g_stdio->in_h == INVALID_HANDLE_VALUE || g_stdio->out_h == INVALID_HANDLE_VALUE) {
    *err = hx::win::LastError("GetStdHandle");
    return false;
  }
  return true;
}

namespace {
void StartStdioThreads() {
  if (g_stdio == nullptr || g_stdio->reader.joinable()) return;
  g_stdio->reader = std::thread([] { g_stdio->ReaderLoop(); });
  g_stdio->writer = std::thread([] { g_stdio->WriterLoop(); });
}
}  // namespace

long Read(Fd fd, void* buf, size_t len) {
  if (fd == kStdin) {
    if (g_stdio == nullptr) return kIoError;
    std::unique_lock<std::mutex> lk(g_stdio->mu);
    if (g_stdio->in_buf.empty()) return g_stdio->in_eof ? 0 : kWouldBlock;
    const size_t take = len < g_stdio->in_buf.size() ? len : g_stdio->in_buf.size();
    std::memcpy(buf, g_stdio->in_buf.data(), take);
    g_stdio->in_buf.erase(0, take);
    const bool drained = g_stdio->in_buf.size() < StdioBridge::kInCap;
    lk.unlock();
    if (drained) g_stdio->cv_reader.notify_one();
    return static_cast<long>(take);
  }
  Stream* s = Get(fd);
  if (s == nullptr || s->closed) return kIoError;
  if (s->rconsumed < s->ravail) {
    const size_t have = s->ravail - s->rconsumed;
    const size_t take = len < have ? len : have;
    std::memcpy(buf, s->rbuf.data() + s->rconsumed, take);
    s->rconsumed += take;
    if (s->rconsumed >= s->ravail) PostRead(fd, s);  // drained; queue the next read at once
    return static_cast<long>(take);
  }
  if (s->eof) return 0;
  if (s->rerror) return kIoError;
  return kWouldBlock;
}

long Write(Fd fd, const void* buf, size_t len) {
  if (fd == kStdout) {
    if (g_stdio == nullptr) return kIoError;
    {
      std::lock_guard<std::mutex> lk(g_stdio->mu);
      if (g_stdio->out_broken) return kIoError;
      if (g_stdio->out_buf.size() >= StdioBridge::kOutCap) return kWouldBlock;
      g_stdio->out_buf.append(static_cast<const char*>(buf), len);
    }
    g_stdio->cv_writer.notify_one();
    return static_cast<long>(len);
  }
  Stream* s = Get(fd);
  if (s == nullptr || s->closed) return kIoError;
  if (s->werror) return kIoError;
  // Push back while a write is in flight: the layer above will turn on
  // "watch for writable" and get called again once it completes. This is
  // exactly what that layer does when a POSIX non-blocking write returns
  // EAGAIN.
  if (s->write_pending) return kWouldBlock;
  s->wbuf.assign(static_cast<const char*>(buf), len);
  s->wsent = 0;
  PostWrite(fd, s);
  if (s->werror) return kIoError;
  return static_cast<long>(len);
}

void Close(Fd fd) {
  if (fd == kStdin || fd == kStdout) return;  // lifetime belongs to StdioBridge
  Stream* s = Get(fd);
  if (s == nullptr) return;
  s->closed = true;
  s->watched_read = false;
  s->watched_write = false;
  if (s->h != INVALID_HANDLE_VALUE) {
    // CancelIo only cancels I/O issued by **this thread** -- all our reads and
    // writes are issued on the single event-loop thread, so it suffices.
    ::CancelIo(s->h);
    ::CloseHandle(s->h);
    s->h = INVALID_HANDLE_VALUE;
  }
  // ★ Never delete here.
  //
  //   CancelIo and CloseHandle are both **asynchronous**: operations in flight
  //   complete with ERROR_OPERATION_ABORTED, their completion packets still
  //   arrive at the IOCP, and on completion the kernel writes status into that
  //   OVERLAPPED -- which lives inside the Stream. Freeing the Stream at this
  //   point hands the kernel memory already returned to the heap: classic
  //   intermittent corruption, and the crash lands somewhere entirely
  //   unrelated.
  //
  //   So keep the husk and reap it once Wait has collected the last
  //   completion packet (see MaybeReap). The slot has to stay too: the
  //   completion key *is* the Fd, and clearing it early loses the way back to
  //   this Stream.
  MaybeReap(fd, s);
}

// ---------------------------------------------------------------- Reactor

struct Reactor::Impl {
  std::map<Fd, bool> watch_write;  // fd -> whether writability is watched
};

Reactor::Reactor() : impl_(new Impl) {}

Reactor::~Reactor() {
  if (g_stdio != nullptr) {
    {
      std::lock_guard<std::mutex> lk(g_stdio->mu);
      g_stdio->stopping = true;
    }
    g_stdio->cv_writer.notify_all();
    g_stdio->cv_reader.notify_all();
    // ★ Do not join the reader thread: it may be blocked in ReadFile while
    //   the host has not closed stdin yet, and joining would hang the exit
    //   path. The process is about to end, so detaching is the right call.
    if (g_stdio->reader.joinable()) g_stdio->reader.detach();
    if (g_stdio->writer.joinable()) g_stdio->writer.join();  // the writer must flush its backlog
  }
  delete impl_;
}

bool Reactor::Open(std::string* err) {
  g_iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (g_iocp == nullptr) {
    *err = hx::win::LastError("CreateIoCompletionPort");
    return false;
  }
  StartStdioThreads();
  return true;
}

bool Reactor::AddRead(Fd fd) {
  if (fd == kStdin) return true;  // stdin is driven by the bridge thread, not the IOCP
  Stream* s = Get(fd);
  if (s == nullptr) return false;
  s->watched_read = true;
  PostRead(fd, s);
  return true;
}

bool Reactor::WatchWrite(Fd fd, bool on) {
  impl_->watch_write[fd] = on;
  if (fd != kStdout) {
    Stream* s = Get(fd);
    if (s != nullptr) s->watched_write = on;
  }
  return true;
}

void Reactor::Del(Fd fd) {
  impl_->watch_write.erase(fd);
  Stream* s = Get(fd);
  if (s != nullptr) {
    s->watched_read = false;
    s->watched_write = false;
  }
}

int Reactor::Wait(int timeout_ms, Event* out, int max, std::string* err) {
  std::map<Fd, Event> ready;

  auto mark = [&ready](Fd fd, bool r, bool w) {
    Event& e = ready[fd];
    e.fd = fd;
    e.readable = e.readable || r;
    e.writable = e.writable || w;
  };

  // 1. Level-triggered: while the buffer still holds data nobody has taken,
  //    we must keep reporting readable. engine.cpp's OnCellFdReadable reads at
  //    most 8 rounds before yielding, and relies on being called back after
  //    it yields.
  for (size_t i = 2; i < g_streams.size(); ++i) {
    Stream* s = g_streams[i];
    if (s == nullptr || s->closed) continue;
    const Fd fd = static_cast<Fd>(i);
    if (s->watched_read && s->ReadableNow()) mark(fd, true, false);
    if (s->watched_write && s->WritableNow()) mark(fd, false, true);
  }
  if (g_stdio != nullptr) {
    std::lock_guard<std::mutex> lk(g_stdio->mu);
    if (!g_stdio->in_buf.empty() || g_stdio->in_eof) mark(kStdin, true, false);
    auto it = impl_->watch_write.find(kStdout);
    if (it != impl_->watch_write.end() && it->second &&
        g_stdio->out_buf.size() < StdioBridge::kOutCap) {
      mark(kStdout, false, true);
    }
  }

  // If something is already ready, do not wait -- just drain whatever
  // completion packets the IOCP already has
  const DWORD wait_ms =
      !ready.empty() ? 0 : (timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));

  bool first = true;
  for (;;) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = nullptr;
    const BOOL ok =
        ::GetQueuedCompletionStatus(g_iocp, &bytes, &key, &ov, first ? wait_ms : 0);
    first = false;
    if (ok == 0 && ov == nullptr) {
      const DWORD e = ::GetLastError();
      if (e == WAIT_TIMEOUT) break;
      *err = "GetQueuedCompletionStatus: " + hx::win::ErrorMessage(e);
      return -1;
    }

    const Fd fd = static_cast<Fd>(key);
    if (ov == nullptr) {
      // A wake-up packet from StdioBridge: re-examine both standard streams
      if (g_stdio != nullptr) {
        std::lock_guard<std::mutex> lk(g_stdio->mu);
        if (!g_stdio->in_buf.empty() || g_stdio->in_eof) mark(kStdin, true, false);
        auto it = impl_->watch_write.find(kStdout);
        if (it != impl_->watch_write.end() && it->second) mark(kStdout, false, true);
      }
      continue;
    }

    Stream* s = Get(fd);
    if (s == nullptr) continue;  // already fully reaped

    // ★ Even for a closed stream, **clear the pending flag first** before
    //   discarding this packet. A bare continue would leave read_pending /
    //   write_pending true forever, MaybeReap would never reap, and the
    //   Stream leaks along with its slot.
    if (s->closed) {
      if (ov == &s->ov_read) s->read_pending = false;
      if (ov == &s->ov_write) s->write_pending = false;
      MaybeReap(fd, s);
      continue;
    }

    if (ov == &s->ov_read) {
      s->read_pending = false;
      if (ok == 0) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_HANDLE_EOF ||
            e == ERROR_OPERATION_ABORTED) {
          s->eof = true;
        } else {
          s->rerror = true;
        }
      } else if (bytes == 0) {
        s->eof = true;  // the other end of the pipe closed
      } else {
        s->ravail = bytes;
        s->rconsumed = 0;
      }
      if (s->watched_read) mark(fd, true, false);
    } else if (ov == &s->ov_write) {
      s->write_pending = false;
      if (ok == 0) {
        s->werror = true;
      } else {
        s->wsent += bytes;
        if (s->wsent < s->wbuf.size()) {
          PostWrite(fd, s);  // not finished; post the rest
        } else {
          s->wbuf.clear();
          s->wsent = 0;
        }
      }
      if (s->watched_write && s->WritableNow()) mark(fd, false, true);
    }
  }

  int n = 0;
  for (const auto& [fd, e] : ready) {
    if (n >= max) break;
    out[n++] = e;
  }
  return n;
}

// ---------------------------------------------------------------- win extras

namespace win {

bool CreatePipePair(bool parent_reads, Fd* parent, HANDLE* child_end, std::string* err) {
  static std::atomic<unsigned> seq{0};
  char name[128];
  ::snprintf(name, sizeof(name), "\\\\.\\pipe\\hx-%lu-%u",
             static_cast<unsigned long>(::GetCurrentProcessId()), seq.fetch_add(1) + 1);
  const std::wstring wname = hx::win::Widen(name);

  // Parent end: overlapped, single instance. PIPE_REJECT_REMOTE_CLIENTS rules
  // out the possibility of a remote connection.
  HANDLE server = ::CreateNamedPipeW(
      wname.c_str(),
      (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED |
          FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      /*nMaxInstances=*/1, static_cast<DWORD>(kChunk), static_cast<DWORD>(kChunk), 0, nullptr);
  if (server == INVALID_HANDLE_VALUE) {
    *err = hx::win::LastError("CreateNamedPipe");
    return false;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;  // this end is inherited by the child
  HANDLE client = ::CreateFileW(wname.c_str(), parent_reads ? GENERIC_WRITE : GENERIC_READ,
                                0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (client == INVALID_HANDLE_VALUE) {
    *err = hx::win::LastError("CreateFile(pipe client)");
    ::CloseHandle(server);
    return false;
  }

  // The client's CreateFile already succeeded, so the connection is in fact
  // established; ConnectNamedPipe then returns ERROR_PIPE_CONNECTED, which
  // means "already connected", not an error.
  OVERLAPPED cov{};
  if (::ConnectNamedPipe(server, &cov) == 0) {
    const DWORD e = ::GetLastError();
    if (e != ERROR_PIPE_CONNECTED && e != ERROR_IO_PENDING && e != ERROR_NO_DATA) {
      *err = "ConnectNamedPipe: " + hx::win::ErrorMessage(e);
      ::CloseHandle(server);
      ::CloseHandle(client);
      return false;
    }
  }

  // ★ A handle can be associated with an IOCP exactly once, and the
  //   completion key is fixed at association time -- associating again gives
  //   ERROR_INVALID_PARAMETER, not "update the key". So Install must come
  //   first to obtain the Fd, and only then can it be used as the key. The
  //   order cannot be reversed.
  auto* s = new Stream();
  s->h = server;
  const Fd fd = Install(s);
  // The completion key is the Fd; Wait uses it to find the Stream again
  if (::CreateIoCompletionPort(server, g_iocp, static_cast<ULONG_PTR>(fd), 0) == nullptr) {
    *err = hx::win::LastError("CreateIoCompletionPort(pipe)");
    Close(fd);
    ::CloseHandle(client);
    return false;
  }
  *parent = fd;
  *child_end = client;
  return true;
}

Fd AdoptHandle(HANDLE h, bool for_read, bool for_write, std::string* err) {
  auto* s = new Stream();
  s->h = h;
  const Fd fd = Install(s);
  if (::CreateIoCompletionPort(h, g_iocp, static_cast<ULONG_PTR>(fd), 0) == nullptr) {
    *err = hx::win::LastError("CreateIoCompletionPort(adopt)");
    g_streams[static_cast<size_t>(fd)] = nullptr;
    delete s;
    return kInvalid;
  }
  s->watched_read = for_read;
  s->watched_write = for_write;
  return fd;
}

HANDLE RawHandle(Fd fd) {
  Stream* s = Get(fd);
  return s == nullptr ? INVALID_HANDLE_VALUE : s->h;
}

}  // namespace win
}  // namespace hx::io
#endif  // _WIN32
