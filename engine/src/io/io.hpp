// I/O 与事件循环的平台 seam。
//
// engine.cpp 的循环逻辑（超时、回收、背压、丢事件）与操作系统无关，
// 但原来的写法把它钉死在 epoll + fd 上。这一层把「等哪些流可读可写」
// 抽出来，两边各自实现：
//
//   Linux   —— epoll，Fd 就是真的文件描述符，Read/Write 是 ::read/::write
//   Windows —— IOCP + overlapped 命名管道，Fd 是流表的下标
//
// ★ 语义以 POSIX 非阻塞 fd 为准（因为 engine.cpp 就是照这个写的）：
//     Read  返回 >0 = 读到字节，0 = EOF，kWouldBlock = 暂时没有，kIoError = 坏了
//     Write 返回 >=0 = 接收了多少字节，kWouldBlock = 一个字节都收不下
//   Windows 端负责把 overlapped 的完成模型伪装成这个样子，
//   而不是让 engine.cpp 去分两套路径写 —— 分两套就等于有两个循环要维护。
#pragma once

#include <cstddef>
#include <string>

namespace hx::io {

/**
 * 流句柄。
 *
 * ★ 故意不叫 fd：在 Windows 上它不是文件描述符，而是流表下标。
 *   名字里带 fd 会诱导人直接拿它去调 ::read —— 那在 Windows 上是错的。
 */
using Fd = int;

inline constexpr Fd kInvalid = -1;

/** 与宿主之间的两条标准流。Windows 上它们走独立实现，见 io_win.cpp 的说明。 */
inline constexpr Fd kStdin = 0;
inline constexpr Fd kStdout = 1;

/** Read/Write 的非字节数返回值。 */
inline constexpr long kWouldBlock = -1;
inline constexpr long kIoError = -2;

/** 必须在任何 I/O 之前调用一次：接管 stdin/stdout，登记它们的 Fd。 */
bool InitStdio(std::string* err);

/** 读。语义见文件头。 */
long Read(Fd fd, void* buf, size_t len);

/** 写。语义见文件头；返回值可能小于 len（部分写），调用方负责留住剩下的。 */
long Write(Fd fd, const void* buf, size_t len);

/** 关闭。对 cell 的 stdin 写端调用它 = 给子进程送 EOF。 */
void Close(Fd fd);

/** 一次 Wait 返回的就绪事件。 */
struct Event {
  Fd fd = kInvalid;
  bool readable = false;
  bool writable = false;
};

/**
 * 事件循环的等待器。
 *
 * ★ 水平触发语义：还有没读完的数据时，下一次 Wait 必须继续报 readable。
 *   engine.cpp 的 OnCellFdReadable 每次最多读 8 轮就让出去（防止高产出的
 *   子进程饿死超时与回收），它依赖的就是「让出去之后还会被叫回来」。
 *   边沿触发会让那段代码静默地丢数据。
 */
class Reactor {
 public:
  Reactor();
  ~Reactor();
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  bool Open(std::string* err);

  /** 关注可读。 */
  bool AddRead(Fd fd);

  /** 开/关可写关注。出站缓冲写不动时才打开，写空了立刻关掉。 */
  bool WatchWrite(Fd fd, bool on);

  /** 不再关注。不负责 Close。 */
  void Del(Fd fd);

  /** timeout_ms < 0 表示无限等。返回就绪事件数，-1 = 出错并填 err。 */
  int Wait(int timeout_ms, Event* out, int max, std::string* err);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace hx::io
