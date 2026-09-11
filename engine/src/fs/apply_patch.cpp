#include "fs/apply_patch.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <string_view>

#include "fs/path_guard.hpp"
#include "proto.hpp"

namespace hx {
namespace {

std::vector<std::string> SplitLines(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t nl = s.find('\n', start);
    if (nl == std::string::npos) {
      if (start < s.size()) out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

std::string JoinLines(const std::vector<std::string>& lines, bool trailing_newline) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size() || trailing_newline) out.push_back('\n');
  }
  return out;
}

bool ReadWholeFile(const std::string& path, std::string* out, std::string* err) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *err = std::string("open: ") + ::strerror(errno);
    return false;
  }
  out->clear();
  char buf[65536];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      out->append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    *err = std::string("read: ") + ::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

// 写到同目录的临时文件再 rename：rename 本身是原子的，
// 崩在中途要么是旧内容要么是新内容，不会出现半个文件。
bool WriteAtomic(const std::string& path, const std::string& content, std::string* err) {
  const size_t slash = path.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  std::string tmpl = dir + "/.hx-tmp-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');

  const int fd = ::mkstemp(buf.data());
  if (fd < 0) {
    *err = std::string("mkstemp: ") + ::strerror(errno);
    return false;
  }
  const std::string tmp_path(buf.data());

  size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      *err = std::string("write: ") + ::strerror(errno);
      ::close(fd);
      ::unlink(tmp_path.c_str());
      return false;
    }
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    *err = std::string("fsync: ") + ::strerror(errno);
    ::close(fd);
    ::unlink(tmp_path.c_str());
    return false;
  }
  ::close(fd);
  ::chmod(tmp_path.c_str(), 0644);
  if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
    *err = std::string("rename: ") + ::strerror(errno);
    ::unlink(tmp_path.c_str());
    return false;
  }
  return true;
}

struct Hunk {
  std::vector<std::string> before;  // 上下文 + 被删除
  std::vector<std::string> after;   // 上下文 + 被加入
};

struct ParsedFile {
  std::string kind;  // add | update | delete
  std::string path;
  std::vector<std::string> add_lines;  // kind == add
  std::vector<Hunk> hunks;             // kind == update
};

bool StartsWith(std::string_view s, std::string_view p) { return s.rfind(p, 0) == 0; }

// 在 lines 中从 from 起找到与 before 完全一致的一段，返回起点；找不到返回 npos
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

  // ---------- 解析 ----------
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
      if (line.empty()) {  // 空行按空上下文处理
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
    // delete：头之后不该有内容
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

  // ---------- 先全部算完（任何失败都还没落盘） ----------
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
      p.content = JoinLines(f.add_lines, /*trailing_newline=*/true);
    } else if (f.kind == "delete") {
      // 不需要内容
    } else {
      std::string original;
      std::string ferr;
      if (!ReadWholeFile(res.path, &original, &ferr)) {
        r.error_code = err::kBadArgs;
        r.error = "cannot read " + f.path + ": " + ferr;
        return r;
      }
      const bool had_trailing = !original.empty() && original.back() == '\n';
      std::vector<std::string> cur_lines = SplitLines(original);

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
      p.content = JoinLines(cur_lines, had_trailing);
    }
    planned.push_back(std::move(p));
  }

  // ---------- 落盘 ----------
  for (const auto& p : planned) {
    std::string ferr;
    if (p.kind == "delete") {
      if (::unlink(p.abs_path.c_str()) != 0) {
        r.error_code = err::kInternal;
        r.error = "unlink " + p.rel_path + ": " + ::strerror(errno);
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
