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

// On POSIX an Fd really is an fd, so this layer is very nearly the identity --
// which is exactly the intent: the original Linux behaviour changes not one
// byte, and Windows is the side that has to do the disguising.
static_assert(kStdin == STDIN_FILENO, "kStdin must equal STDIN_FILENO on POSIX");
static_assert(kStdout == STDOUT_FILENO, "kStdout must equal STDOUT_FILENO on POSIX");

bool InitStdio(std::string* err) {
  // Writing to stdout after the host disconnects raises SIGPIPE, whose
  // default action kills the process outright -- leaving no chance even to
  // drop the backlog and exit cleanly.
  ::signal(SIGPIPE, SIG_IGN);
  // stdout must be non-blocking, or a slow-reading host can freeze the whole
  // single-threaded engine.
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
    // Treat EPOLLHUP/EPOLLERR as readable too: let the layer above do one
    // read and get 0 or an error from it. The teardown logic lives in exactly
    // one place; do not fork it here.
    out[i].readable = (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
    out[i].writable = (events[i].events & EPOLLOUT) != 0;
  }
  return n;
}

}  // namespace hx::io
#endif  // !_WIN32
