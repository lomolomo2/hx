#ifndef _WIN32
#include "io/io.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hx::io {
namespace {

bool SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

// POSIX 上 Fd 就是 fd，这一层几乎是恒等映射 —— 这正是想要的：
// 原来的 Linux 行为一个字节都没变，Windows 才是需要伪装的那一边。
static_assert(kStdin == STDIN_FILENO, "kStdin must equal STDIN_FILENO on POSIX");
static_assert(kStdout == STDOUT_FILENO, "kStdout must equal STDOUT_FILENO on POSIX");

bool InitStdio(std::string* err) {
  // 宿主断开时写 stdout 会收到 SIGPIPE，默认动作是直接杀掉进程 ——
  // 那样连"把积压丢掉、干净退出"的机会都没有。
  ::signal(SIGPIPE, SIG_IGN);
  // stdout 必须非阻塞，否则读得慢的宿主能把整个单线程引擎冻住。
  if (!SetNonBlocking(STDOUT_FILENO)) {
    *err = std::string("fcntl(stdout, O_NONBLOCK): ") + ::strerror(errno);
    return false;
  }
  return true;
}

long Read(Fd fd, void* buf, size_t len) {
  for (;;) {
    const ssize_t n = ::read(fd, buf, len);
    if (n >= 0) return static_cast<long>(n);
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return kWouldBlock;
    return kIoError;
  }
}

long Write(Fd fd, const void* buf, size_t len) {
  for (;;) {
    const ssize_t n = ::write(fd, buf, len);
    if (n >= 0) return static_cast<long>(n);
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return kWouldBlock;
    return kIoError;
  }
}

void Close(Fd fd) {
  if (fd >= 0) ::close(fd);
}

struct Reactor::Impl {
  int ep = -1;
};

Reactor::Reactor() : impl_(new Impl) {}

Reactor::~Reactor() {
  if (impl_ != nullptr) {
    if (impl_->ep >= 0) ::close(impl_->ep);
    delete impl_;
  }
}

bool Reactor::Open(std::string* err) {
  impl_->ep = ::epoll_create1(EPOLL_CLOEXEC);
  if (impl_->ep < 0) {
    *err = std::string("epoll_create1: ") + ::strerror(errno);
    return false;
  }
  return true;
}

bool Reactor::AddRead(Fd fd) {
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  return ::epoll_ctl(impl_->ep, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Reactor::WatchWrite(Fd fd, bool on) {
  if (!on) return ::epoll_ctl(impl_->ep, EPOLL_CTL_DEL, fd, nullptr) == 0;
  epoll_event ev{};
  ev.events = EPOLLOUT;
  ev.data.fd = fd;
  return ::epoll_ctl(impl_->ep, EPOLL_CTL_ADD, fd, &ev) == 0;
}

void Reactor::Del(Fd fd) { ::epoll_ctl(impl_->ep, EPOLL_CTL_DEL, fd, nullptr); }

int Reactor::Wait(int timeout_ms, Event* out, int max, std::string* err) {
  epoll_event events[64];
  const int cap = max < 64 ? max : 64;
  const int n = ::epoll_wait(impl_->ep, events, cap, timeout_ms);
  if (n < 0) {
    if (errno == EINTR) return 0;
    *err = std::string("epoll_wait: ") + ::strerror(errno);
    return -1;
  }
  for (int i = 0; i < n; ++i) {
    out[i].fd = events[i].data.fd;
    // EPOLLHUP/EPOLLERR 也当可读：让上层去 read 一次拿到 0 或错误，
    // 收口逻辑只有一处，不在这里分叉。
    out[i].readable = (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
    out[i].writable = (events[i].events & EPOLLOUT) != 0;
  }
  return n;
}

}  // namespace hx::io
#endif  // !_WIN32
