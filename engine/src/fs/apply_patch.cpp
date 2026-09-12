#include "fs/apply_patch.hpp"

#include "platform/platform.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "fs/path_guard.hpp"
#include "proto.hpp"

namespace hx {
namespace {

/**
 * Line-ending style.
 *
 * ★ Not handling CRLF has worse consequences than it sounds, both of them
 *   measured:
 *
 *   1. **Context never matches.** The patch format splits on '\n', while the
 *      overwhelming majority of files on Windows are CRLF -- every line cut
 *      out of the file carries a trailing '\r', so a byte-for-byte comparison
 *      against the patch's context lines is bound to fail. The symptom is
 *      apply_patch reporting "context not found" for every CRLF file on
 *      Windows while the patch itself is perfectly correct.
 *
 *   2. **Even when it matches, writing back rewrites the whole file's line
 *      endings.** Rejoining on '\n' converts a CRLF file wholesale to LF, so
 *      "changed one line" produces a whole-file diff. In a Windows repository
 *      that is enough to drown the real change.
 *
 *   Hence: strip '\r' when splitting, remember the original style, and restore
 *   it when joining back.
 */
enum class LineEnding { kLf, kCrLf };

/** When style is non-null, reports this text's original line-ending style
 *  (judged by the first newline). */
std::vector<std::string> SplitLines(const std::string& s, LineEnding* style = nullptr) {
  if (style != nullptr) {
    const size_t nl = s.find('\n');
    *style = (nl != std::string::npos && nl > 0 && s[nl - 1] == '\r') ? LineEnding::kCrLf
                                                                     : LineEnding::kLf;
  }
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t nl = s.find('\n', start);
    if (nl == std::string::npos) {
      if (start < s.size()) {
        std::string last = s.substr(start);
        if (!last.empty() && last.back() == '\r') last.pop_back();
        out.push_back(std::move(last));
      }
      break;
    }
    size_t len = nl - start;
    if (len > 0 && s[start + len - 1] == '\r') --len;  // strip CRLF's '\r'
    out.push_back(s.substr(start, len));
    start = nl + 1;
  }
  return out;
}

std::string JoinLines(const std::vector<std::string>& lines, bool trailing_newline,
                      LineEnding style) {
  const char* eol = (style == LineEnding::kCrLf) ? "\r\n" : "\n";
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size() || trailing_newline) out += eol;
  }
  return out;
}

// All file I/O goes through platform: the semantics of atomic replacement
// (temp file -> flush -> rename) are identical on both platforms; see
// platform::WriteFileAtomic.
bool ReadWholeFile(const std::string& path, std::string* out, std::string* err) {
  return platform::ReadWholeFile(path, out, err);
}

bool WriteAtomic(const std::string& path, const std::string& content, std::string* err) {
  return platform::WriteFileAtomic(path, content, err);
}

struct Hunk {
  std::vector<std::string> before;  // context + removed lines
  std::vector<std::string> after;   // context + added lines
};

struct ParsedFile {
  std::string kind;  // add | update | delete
  std::string path;
  std::vector<std::string> add_lines;  // kind == add
  std::vector<Hunk> hunks;             // kind == update
};

bool StartsWith(std::string_view s, std::string_view p) { return s.rfind(p, 0) == 0; }

// Find a run in lines, starting at from, that matches before exactly, and
// return its start; npos when not found
size_t FindBlock(const std::vector<std::string>& lines, const std::vector<std::string>& before,
                 size_t from) {
  if (before.empty()) return from;
  if (before.size() > lines.size()) return std::string::npos;
  for (size_t i = from; i + before.size() <= lines.size(); ++i) {
    bool hit = true;
    for (size_t j = 0; j < before.size(); ++j) {
      if (lines[i + j] != before[j]) { hit = false; break; }
    }
    if (hit) return i;
  }
  return std::string::npos;
}

}  // namespace

ApplyPatchResult ApplyPatch(const std::string& patch_text, const std::string& base,
                            const std::vector<std::string>& roots, bool allow_write) {
  ApplyPatchResult r;
  if (!allow_write) {
    r.error_code = err::kDenied;
    r.error = "sandbox is read-only; apply_patch denied";
    return r;
  }

  // ---------- parse ----------
  const std::vector<std::string> lines = SplitLines(patch_text);
  std::vector<ParsedFile> files;
  ParsedFile* cur = nullptr;
  Hunk* hunk = nullptr;

  for (const auto& line : lines) {
    if (StartsWith(line, "*** Begin Patch") || StartsWith(line, "*** End Patch")) {
      continue;
    }
    if (StartsWith(line, "*** Add File: ") || StartsWith(line, "*** Update File: ") ||
        StartsWith(line, "*** Delete File: ")) {
      files.emplace_back();
      cur = &files.back();
      hunk = nullptr;
      if (StartsWith(line, "*** Add File: ")) {
        cur->kind = "add";
        cur->path = line.substr(std::strlen("*** Add File: "));
      } else if (StartsWith(line, "*** Update File: ")) {
        cur->kind = "update";
        cur->path = line.substr(std::strlen("*** Update File: "));
      } else {
        cur->kind = "delete";
        cur->path = line.substr(std::strlen("*** Delete File: "));
      }
      continue;
    }
    if (cur == nullptr) {
      if (line.empty()) continue;
      r.error_code = err::kBadArgs;
      r.error = "patch content before any *** File header: " + line;
      return r;
    }
    if (StartsWith(line, "@@")) {
      cur->hunks.emplace_back();
      hunk = &cur->hunks.back();
      continue;
    }
    if (cur->kind == "add") {
      if (!line.empty() && line[0] != '+') {
        r.error_code = err::kBadArgs;
        r.error = "Add File lines must start with '+': " + line;
        return r;
      }
      cur->add_lines.push_back(line.empty() ? std::string() : line.substr(1));
      continue;
    }
    if (cur->kind == "update") {
      if (hunk == nullptr) {
        cur->hunks.emplace_back();
        hunk = &cur->hunks.back();
      }
      if (line.empty()) {  // an empty line counts as empty context
        hunk->before.emplace_back();
        hunk->after.emplace_back();
      } else if (line[0] == ' ') {
        hunk->before.push_back(line.substr(1));
        hunk->after.push_back(line.substr(1));
      } else if (line[0] == '-') {
        hunk->before.push_back(line.substr(1));
      } else if (line[0] == '+') {
        hunk->after.push_back(line.substr(1));
      } else {
        r.error_code = err::kBadArgs;
        r.error = "unexpected patch line (want ' ', '-', '+', '@@'): " + line;
        return r;
      }
      continue;
    }
    // delete: nothing may follow the header
    if (!line.empty()) {
      r.error_code = err::kBadArgs;
      r.error = "Delete File takes no body: " + line;
      return r;
    }
  }

  if (files.empty()) {
    r.error_code = err::kBadArgs;
    r.error = "patch contains no file sections";
    return r;
  }

  // ---------- compute everything first (nothing is written until it all
  //            succeeds) ----------
  struct Planned {
    std::string kind;
    std::string abs_path;
    std::string rel_path;
    std::string content;
    bool trailing_newline = true;
  };
  std::vector<Planned> planned;

  for (const auto& f : files) {
    const bool must_exist = (f.kind != "add");
    const ResolveResult res = ResolveInRoots(f.path, base, roots, must_exist);
    if (!res.ok) {
      r.error_code = err::kPathEscape;
      r.error = res.error;
      return r;
    }

    Planned p;
    p.kind = f.kind;
    p.abs_path = res.path;
    p.rel_path = f.path;

    if (f.kind == "add") {
      // Newly created files always use LF, identically on both platforms.
      // The reason is determinism: the patch itself is LF, and following the
      // platform would make the same patch produce byte-different files on
      // Linux and Windows, with tests and diffs drifting along with them.
      p.content = JoinLines(f.add_lines, /*trailing_newline=*/true, LineEnding::kLf);
    } else if (f.kind == "delete") {
      // no content needed
    } else {
      std::string original;
      std::string ferr;
      if (!ReadWholeFile(res.path, &original, &ferr)) {
        r.error_code = err::kBadArgs;
        r.error = "cannot read " + f.path + ": " + ferr;
        return r;
      }
      const bool had_trailing = !original.empty() && original.back() == '\n';
      // Remember the original file's line-ending style and restore it verbatim
      // when writing back -- see the comments on LineEnding.
      LineEnding style = LineEnding::kLf;
      std::vector<std::string> cur_lines = SplitLines(original, &style);

      size_t cursor = 0;
      for (const auto& h : f.hunks) {
        if (h.before.empty() && h.after.empty()) continue;
        const size_t at = FindBlock(cur_lines, h.before, cursor);
        if (at == std::string::npos) {
          r.error_code = err::kBadArgs;
          r.error = "hunk does not apply to " + f.path + " (context not found)";
          return r;
        }
        cur_lines.erase(cur_lines.begin() + static_cast<long>(at),
                        cur_lines.begin() + static_cast<long>(at + h.before.size()));
        cur_lines.insert(cur_lines.begin() + static_cast<long>(at), h.after.begin(),
                         h.after.end());
        cursor = at + h.after.size();
      }
      p.content = JoinLines(cur_lines, had_trailing, style);
    }
    planned.push_back(std::move(p));
  }

  // ---------- write ----------
  for (const auto& p : planned) {
    std::string ferr;
    if (p.kind == "delete") {
      if (!platform::Unlink(p.abs_path)) {
        r.error_code = err::kInternal;
        r.error = "unlink " + p.rel_path;
        return r;
      }
    } else if (!WriteAtomic(p.abs_path, p.content, &ferr)) {
      r.error_code = err::kInternal;
      r.error = "write " + p.rel_path + ": " + ferr;
      return r;
    }
    r.changes.push_back(PatchChange{p.rel_path, p.kind});
  }

  r.ok = true;
  return r;
}

}  // namespace hx
