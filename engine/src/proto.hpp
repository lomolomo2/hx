// hxp v0 —— 协议类型与编解码。规范见 proto/hxp-v0.md
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace hx {

using json = nlohmann::json;

// 单行上限，超限即拒（proto/hxp-v0.md §0）
inline constexpr size_t kMaxLineBytes = 8u * 1024u * 1024u;

namespace err {
inline constexpr const char* kBadRequest = "E_BAD_REQUEST";
inline constexpr const char* kUnknownOp = "E_UNKNOWN_OP";
inline constexpr const char* kBadArgs = "E_BAD_ARGS";
inline constexpr const char* kPathEscape = "E_PATH_ESCAPE";
inline constexpr const char* kDenied = "E_DENIED";
inline constexpr const char* kNoSession = "E_NO_SESSION";
inline constexpr const char* kNoCell = "E_NO_CELL";
inline constexpr const char* kLineTooLong = "E_LINE_TOO_LONG";
inline constexpr const char* kSandboxUnavailable = "E_SANDBOX_UNAVAILABLE";
inline constexpr const char* kInternal = "E_INTERNAL";
}  // namespace err

// 崩溃恢复语义，不是重试策略（proto/hxp-v0.md §1）
enum class Replay { kNever, kSafe };

struct Request {
  std::string id;
  std::string op;
  json args = json::object();
  Replay replay = Replay::kNever;
};

struct ParseError {
  std::string code;
  std::string message;
  // id 可能已解析出来；解析不出时为空，此时响应无法关联，只能发事件
  std::string id;
};

// 解析一行。成功写入 out 并返回 true；失败写入 e 并返回 false。
//
// 契约（proto/hxp-v0.md §5 参数捕获）：返回的 Request 持有 args 的**独立副本**，
// 调用方此后不得再引用解析用的输入缓冲区。
bool ParseRequest(const std::string& line, Request* out, ParseError* e);

json MakeOk(const std::string& id, json result);
json MakeError(const std::string& id, std::string code, std::string message,
               json detail = json(nullptr));
json MakeEvent(std::string name, json fields);

}  // namespace hx
