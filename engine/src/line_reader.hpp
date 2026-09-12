// Split stdin into lines. An over-long line is discarded whole and reported as
// an error, but the connection is not dropped (proto/hxp-v0.md section 0).
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "proto.hpp"

namespace hx {

class LineReader {
 public:
  void Feed(std::string_view chunk, const std::function<void(std::string_view)>& on_line,
            const std::function<void()>& on_oversize) {
    while (!chunk.empty()) {
      const size_t nl = chunk.find('\n');
      if (nl == std::string_view::npos) {
        Append(chunk, on_oversize);
        return;
      }
      Append(chunk.substr(0, nl), on_oversize);
      chunk.remove_prefix(nl + 1);

      if (discarding_) {
        discarding_ = false;  // the over-long line's terminator arrived; resume normally
        buf_.clear();
        continue;
      }
      on_line(buf_);
      buf_.clear();
    }
  }

 private:
  void Append(std::string_view piece, const std::function<void()>& on_oversize) {
    if (discarding_) return;
    if (buf_.size() + piece.size() > kMaxLineBytes) {
      discarding_ = true;
      buf_.clear();
      on_oversize();
      return;
    }
    buf_.append(piece);
  }

  std::string buf_;
  bool discarding_ = false;
};

}  // namespace hx
