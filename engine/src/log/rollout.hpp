// rollout —— append-only 的会话日志，是这个系统里的"事实来源"。
//
// ★ 路径由引擎决定，宿主只能给一个受校验的会话名。
//   否则宿主就能用 log.append 往任意文件追加内容（例如 shell 的 rc 文件），
//   等于在架构上开了个后门 —— 引擎唯一能写的地方必须是它自己划定的。
//
// 持久性承诺（要说准，不能含糊）：
//   · 每条记录用一次 write() 追加（O_APPEND），因此进程被 kill -9 也不会留下半行
//   · 掉电级别的持久性需要 fsync，只在 Flush() 时做
#pragma once

#include "platform/platform.hpp"

#include <cstdint>
#include <string>

#include "proto.hpp"

namespace hx {

class Rollout {
 public:
  ~Rollout();

  // base 为空时用 $HOME/.hx/sessions。name 必须是 [A-Za-z0-9_-]{1,64}。
  bool Open(const std::string& base, const std::string& name, std::string* err);
  // 返回本条记录的序号；未打开时返回 -1
  int64_t Append(const json& record);
  bool Flush();
  void Close();

  const std::string& path() const { return path_; }
  bool is_open() const { return file_.valid(); }
  int64_t seq() const { return seq_; }

  static bool IsValidName(const std::string& name);

 private:
  platform::AppendFile file_;
  std::string path_;
  int64_t seq_ = 0;
};

}  // namespace hx
