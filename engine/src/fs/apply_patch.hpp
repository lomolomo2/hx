// apply_patch —— 补丁式编辑，而不是整文件覆写。
//
// 为什么不做 fs.write：补丁省 token、可 diff、可审批、可回滚，
// 而且模型写整文件时极易悄悄丢掉它"没注意到"的部分。
//
// 格式（codex 风格的子集）：
//   *** Begin Patch
//   *** Add File: a/b.txt
//   +新文件的每一行都以 + 开头
//   *** Update File: src/x.cpp
//   @@ 可选的定位提示
//    上下文行以空格开头
//   -被删除的行
//   +被加入的行
//   *** Delete File: old.txt
//   *** End Patch
#pragma once

#include <string>
#include <vector>

namespace hx {

struct PatchChange {
  std::string path;  // 相对 base 的原始写法，用于回报
  std::string kind;  // "add" | "update" | "delete"
};

struct ApplyPatchResult {
  bool ok = false;
  std::string error_code;  // 失败时对应 proto 的错误码
  std::string error;
  std::vector<PatchChange> changes;
};

// 全部算完再落盘：任何一步失败都不写任何文件。
ApplyPatchResult ApplyPatch(const std::string& patch_text, const std::string& base,
                            const std::vector<std::string>& roots, bool allow_write);

}  // namespace hx
