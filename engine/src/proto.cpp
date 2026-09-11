#include "proto.hpp"

namespace hx {

bool ParseRequest(const std::string& line, Request* out, ParseError* e) {
  json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    *e = {err::kBadRequest, "line is not a JSON object", ""};
    return false;
  }

  // id 先解析出来：后续任何错误都要能关联回请求
  std::string id;
  if (auto it = j.find("id"); it != j.end() && it->is_string()) {
    id = it->get<std::string>();
  }
  if (id.empty()) {
    *e = {err::kBadRequest, "missing or non-string field: id", ""};
    return false;
  }

  auto op_it = j.find("op");
  if (op_it == j.end() || !op_it->is_string()) {
    *e = {err::kBadRequest, "missing or non-string field: op", id};
    return false;
  }

  Request req;
  req.id = id;
  req.op = op_it->get<std::string>();

  if (auto it = j.find("args"); it != j.end()) {
    if (!it->is_object()) {
      *e = {err::kBadArgs, "args must be an object", id};
      return false;
    }
    req.args = *it;  // 深拷贝：参数捕获，见 proto.hpp 契约
  }

  if (auto it = j.find("replay"); it != j.end()) {
    if (!it->is_string()) {
      *e = {err::kBadArgs, "replay must be a string", id};
      return false;
    }
    const std::string r = it->get<std::string>();
    if (r == "safe") {
      req.replay = Replay::kSafe;
    } else if (r == "never") {
      req.replay = Replay::kNever;
    } else {
      *e = {err::kBadArgs, "replay must be \"never\" or \"safe\"", id};
      return false;
    }
  }

  *out = std::move(req);
  return true;
}

json MakeOk(const std::string& id, json result) {
  return json{{"reply_to", id}, {"ok", true}, {"result", std::move(result)}};
}

json MakeError(const std::string& id, std::string code, std::string message, json detail) {
  json e{{"code", std::move(code)}, {"message", std::move(message)}};
  if (!detail.is_null()) {
    e["detail"] = std::move(detail);
  }
  return json{{"reply_to", id}, {"ok", false}, {"error", std::move(e)}};
}

json MakeEvent(std::string name, json fields) {
  json ev = std::move(fields);
  ev["event"] = std::move(name);
  return ev;
}

}  // namespace hx
