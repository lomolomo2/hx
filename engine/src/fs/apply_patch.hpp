// apply_patch -- patch-based editing rather than whole-file overwrites.
//
// Why there is no fs.write: patches cost fewer tokens, can be diffed,
// approved and rolled back, and a model writing a whole file very easily drops
// the parts it "did not notice" without saying so.
//
// The format (a subset of codex's):
//   *** Begin Patch
//   *** Add File: a/b.txt
//   +every line of a new file starts with +
//   *** Update File: src/x.cpp
//   @@ an optional locating hint
//    context lines start with a space
//   -a removed line
//   +an added line
//   *** Delete File: old.txt
//   *** End Patch
#pragma once

#include <string>
#include <vector>

namespace hx {

struct PatchChange {
  std::string path;  // the original spelling relative to base, for reporting
  std::string kind;  // "add" | "update" | "delete"
};

struct ApplyPatchResult {
  bool ok = false;
  std::string error_code;  // on failure, the matching proto error code
  std::string error;
  std::vector<PatchChange> changes;
};

// Everything is computed before anything is written: if any step fails, no
// file is written at all.
ApplyPatchResult ApplyPatch(const std::string& patch_text, const std::string& base,
                            const std::vector<std::string>& roots, bool allow_write);

}  // namespace hx
