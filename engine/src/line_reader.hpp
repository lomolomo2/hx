// 按行切分 stdin。超长行整行丢弃并报错，但不断开连接（proto/hxp-v0.md §0）。
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
        discarding_ = false;  // 超长行的行尾到了，恢复正常
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
