// hxp v0 -- protocol types plus encoding and decoding. The spec is in
// proto/hxp-v0.md
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace hx {

using json = nlohmann::json;

// Per-line cap; anything over it is rejected (proto/hxp-v0.md section 0)
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

// Crash-recovery semantics, not a retry policy (proto/hxp-v0.md section 1)
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
  // The id may already have been parsed; empty when it could not be, in which
  // case a response cannot be correlated and only an event can be sent
  std::string id;
};

// Parse one line. On success writes to out and returns true; on failure writes
// to e and returns false.
//
// The contract (proto/hxp-v0.md section 5, parameter capture): the returned
// Request holds an **independent copy** of args, and the caller must not refer
// to the input buffer used for parsing afterwards.
bool ParseRequest(const std::string& line, Request* out, ParseError* e);

json MakeOk(const std::string& id, json result);
json MakeError(const std::string& id, std::string code, std::string message,
               json detail = json(nullptr));
json MakeEvent(std::string name, json fields);

}  // namespace hx
