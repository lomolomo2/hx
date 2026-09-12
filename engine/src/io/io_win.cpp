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

// ★ 注意 hx::win 与 hx::io::win 是两个不同的命名空间。
//   在 namespace hx::io 里写 win::Foo 会解析到后者（本文件末尾那个），
//   所以调用 win_util 的东西一律写全 hx::win::。
#include "platform/win_util.hpp"

namespace hx::io {
namespace {

constexpr size_t kChunk = 64u * 1024u;

// ★ 全进程一个 IOCP。
//   引擎本来就只有一个事件循环（engine.cpp 的注释写得很清楚：单线程、无锁），
//   所以这里不做成 per-Reactor 实例 —— 那只会逼着 CreatePipePair 到处传
//   Reactor&，换来一个永远不会用到的灵活性。
HANDLE g_iocp = nullptr;

/**
 * 一条流。
 *
 * Windows 的 overlapped 是「完成」模型，engine.cpp 要的是 POSIX 的「就绪」模型。
 * 差值就存在这里：我们始终预投一个 ReadFile，完成的数据落进 rbuf，
 * 上层 Read() 从 rbuf 拿 —— 于是上层看到的就是一个非阻塞 fd。
 */
struct Stream {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool watched_read = false;
  bool watched_write = false;
  bool closed = false;

  // ---- 读侧 ----
  OVERLAPPED ov_read{};
  std::vector<char> rbuf;
  size_t ravail = 0;     // rbuf 中有效字节数
  size_t rconsumed = 0;  // 上层已取走多少
  bool read_pending = false;
  bool eof = false;
  bool rerror = false;

  // ---- 写侧 ----
  OVERLAPPED ov_write{};
  std::string wbuf;   // 已接收、尚未落盘的字节
  size_t wsent = 0;   // wbuf 中已写出多少
  bool write_pending = false;
  bool werror = false;

  bool ReadableNow() const {
    return (ravail > rconsumed) || eof || rerror;
  }
  bool WritableNow() const { return !write_pending; }
};

// 流表。下标即 Fd；0/1 永远留给 stdin/stdout（它们走 StdioBridge，没有 Stream）。
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
 * 已关闭、且再没有在途 I/O 的流，到这里才真正释放。
 *
 * 见 Close() 里的说明：完成包会带着指向 Stream 内部 OVERLAPPED 的指针回来，
 * 早一步释放就是把内存交给内核去写。
 */
void MaybeReap(Fd fd, Stream* s) {
  if (!s->closed || s->read_pending || s->write_pending) return;
  g_streams[static_cast<size_t>(fd)] = nullptr;
  delete s;
}

/** 投一个读。已有在途的读则什么都不做。 */
void PostRead(Fd fd, Stream* s) {
  if (s->read_pending || s->eof || s->rerror || s->closed) return;
  if (s->rconsumed < s->ravail) return;  // 缓冲还没取空，不覆盖
  s->ravail = 0;
  s->rconsumed = 0;
  if (s->rbuf.size() < kChunk) s->rbuf.resize(kChunk);
  ::ZeroMemory(&s->ov_read, sizeof(s->ov_read));
  s->read_pending = true;
  if (::ReadFile(s->h, s->rbuf.data(), static_cast<DWORD>(kChunk), nullptr, &s->ov_read) == 0) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_IO_PENDING) return;  // 正常路径
    s->read_pending = false;
    if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_HANDLE_EOF) {
      s->eof = true;
    } else {
      s->rerror = true;
    }
  }
  // 成功同步完成时完成包仍会入队（句柄已关联 IOCP），统一在 Wait 里处理
  (void)fd;
}

/** 把 wbuf 里没写出去的部分投出去。 */
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
// ★ 这是整个 Windows 移植里唯一不得不引入线程的地方，值得说清楚为什么。
//
//   宿主（Node）用 libuv 建 stdio 管道，给子进程的那一端是**同步**句柄 ——
//   libuv 故意这么做，因为普通子进程都假设 stdio 是同步的。同步句柄不能
//   关联 IOCP（CreateIoCompletionPort 会 ERROR_INVALID_PARAMETER），
//   对它 ReadFile 就是阻塞。
//
//   于是只剩三条路：
//     ① 轮询 PeekNamedPipe  —— 无线程，但每个请求都平白多出一个轮询周期的延迟
//     ② 换传输（命名管道）  —— 要改协议，proto/hxp-v0.md 明写 stdio
//     ③ 两个搬字节的线程    —— 选了这个
//
//   代价必须诚实记下来：README 说的「单线程、无锁、无竞态」在 Windows 上
//   不再字面成立。但**引擎状态仍然是单线程的**：这两个线程只搬字节，
//   不碰 cells_/waits_/policy_ 里的任何东西，交接点就是下面这两个队列。
//   竞态面被压到「一个 deque + 一个 mutex」，而不是散落在事件循环里。
struct StdioBridge {
  HANDLE in_h = INVALID_HANDLE_VALUE;
  HANDLE out_h = INVALID_HANDLE_VALUE;

  std::mutex mu;
  std::condition_variable cv_writer;   // 通知写线程有新数据
  std::condition_variable cv_reader;   // 通知读线程可以继续读了

  std::string in_buf;                  // 读线程 -> 主线程
  bool in_eof = false;
  std::string out_buf;                 // 主线程 -> 写线程
  bool out_broken = false;
  bool stopping = false;

  std::thread reader;
  std::thread writer;

  // 背压：宿主猛灌时不能无限缓冲
  static constexpr size_t kInCap = 8u * 1024u * 1024u;
  static constexpr size_t kOutCap = 16u * 1024u * 1024u;

  void Wake() {
    // 只是叫醒事件循环，没有实际 I/O 完成；键用 kStdin，lpOverlapped 为空
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
      // 主线程还没消化完就先别读，免得一个话痨宿主把我们撑爆
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
      Wake();  // 写空了 -> 主线程可以继续灌
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
    if (s->rconsumed >= s->ravail) PostRead(fd, s);  // 取空了，立刻续上下一个读
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
  // 已有在途的写就先顶回去：上层会挂上「关注可写」，写完再叫它。
  // 这正是 POSIX 非阻塞 write 返回 EAGAIN 时上层做的事。
  if (s->write_pending) return kWouldBlock;
  s->wbuf.assign(static_cast<const char*>(buf), len);
  s->wsent = 0;
  PostWrite(fd, s);
  if (s->werror) return kIoError;
  return static_cast<long>(len);
}

void Close(Fd fd) {
  if (fd == kStdin || fd == kStdout) return;  // 生命周期归 StdioBridge
  Stream* s = Get(fd);
  if (s == nullptr) return;
  s->closed = true;
  s->watched_read = false;
  s->watched_write = false;
  if (s->h != INVALID_HANDLE_VALUE) {
    // CancelIo 只取消**本线程**发起的 I/O —— 我们所有的读写都在事件循环
    // 这一个线程上发起，所以够用。
    ::CancelIo(s->h);
    ::CloseHandle(s->h);
    s->h = INVALID_HANDLE_VALUE;
  }
  // ★ 绝不在这里 delete。
  //
  //   CancelIo 与 CloseHandle 都是**异步**的：在途的操作会以
  //   ERROR_OPERATION_ABORTED 完成，完成包照样进 IOCP，而内核在完成时
  //   会往那个 OVERLAPPED（就在 Stream 里）写状态。这时候把 Stream 释放掉，
  //   就是让内核写一块已经还给堆的内存 —— 典型的偶发性破坏，
  //   现场还落在完全无关的地方。
  //
  //   所以留着空壳，等 Wait 里收完最后一个完成包再回收（见 MaybeReap）。
  //   槽位也要留着：完成键就是 Fd，提前置空就找不回这个 Stream 了。
  MaybeReap(fd, s);
}

// ---------------------------------------------------------------- Reactor

struct Reactor::Impl {
  std::map<Fd, bool> watch_write;  // fd -> 是否关注可写
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
    // ★ 不 join 读线程：它可能正阻塞在 ReadFile 上，而宿主还没关 stdin，
    //   join 会挂死在退出路径上。进程就要结束了，detach 是对的选择。
    if (g_stdio->reader.joinable()) g_stdio->reader.detach();
    if (g_stdio->writer.joinable()) g_stdio->writer.join();  // 写线程要把积压刷完
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
  if (fd == kStdin) return true;  // stdin 由桥接线程驱动，不进 IOCP
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

  // ① 水平触发：缓冲里还有没取走的数据，就必须继续报可读。
  //    engine.cpp 的 OnCellFdReadable 每次最多读 8 轮就让出去，
  //    靠的就是「让出去之后还会被叫回来」。
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

  // 已经有就绪的就不再等，只把 IOCP 里现成的完成包一并排空
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
      // StdioBridge 的叫醒包：重新看一眼两条标准流的状态
      if (g_stdio != nullptr) {
        std::lock_guard<std::mutex> lk(g_stdio->mu);
        if (!g_stdio->in_buf.empty() || g_stdio->in_eof) mark(kStdin, true, false);
        auto it = impl_->watch_write.find(kStdout);
        if (it != impl_->watch_write.end() && it->second) mark(kStdout, false, true);
      }
      continue;
    }

    Stream* s = Get(fd);
    if (s == nullptr) continue;  // 已经回收干净了

    // ★ 已关闭的流也必须**先把 pending 标志清掉**再丢弃这个包。
    //   直接 continue 的话 read_pending / write_pending 永远为真，
    //   MaybeReap 就永远不会回收，Stream 和槽位一起泄漏。
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
        s->eof = true;  // 管道对端关了
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
          PostWrite(fd, s);  // 没写完，接着投
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

  // 父端：overlapped，单实例。PIPE_REJECT_REMOTE_CLIENTS 关掉远程连接的可能。
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
  sa.bInheritHandle = TRUE;  // 这一端要被子进程继承
  HANDLE client = ::CreateFileW(wname.c_str(), parent_reads ? GENERIC_WRITE : GENERIC_READ,
                                0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (client == INVALID_HANDLE_VALUE) {
    *err = hx::win::LastError("CreateFile(pipe client)");
    ::CloseHandle(server);
    return false;
  }

  // 客户端已经 CreateFile 成功，连接其实已经建立；
  // ConnectNamedPipe 此时返回 ERROR_PIPE_CONNECTED，那是「已连上」不是错误。
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

  // ★ 一个句柄只能关联一次 IOCP，而且关联时就要定死完成键 ——
  //   再关联一次是 ERROR_INVALID_PARAMETER，不是「更新键」。
  //   所以必须先 Install 拿到 Fd，再拿它当键关联，顺序反不得。
  auto* s = new Stream();
  s->h = server;
  const Fd fd = Install(s);
  // 完成键就是 Fd，Wait 靠它找回 Stream
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
