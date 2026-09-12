// 平台原语。
//
// 这一层存在的唯一理由：engine.cpp / rollout.cpp / path_guard.cpp 里那些
// 「本来就与操作系统无关」的逻辑，不该因为 realpath / mkdir / clock_gettime
// 这种小东西被钉死在 POSIX 上。
//
// ★ 边界要划得准：真正带语义的东西（沙箱、进程组、pty）不在这里，
//   它们各自有自己的 seam（sandbox/confine.hpp、exec/proc.hpp、exec/pty.hpp）。
//   这里只放「两边都有、只是拼写不同」的调用。混进语义就会变成
//   一个什么都往里塞的 util.h。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hx::platform {

/** 单调时钟毫秒。用于所有 deadline —— 绝不能用挂钟，改时间会让超时错乱。 */
int64_t NowMs();

/** 本进程 pid（只用于日志与 ping，协议里从不出现真实 pid）。 */
int64_t Pid();

/** 解析为规范化的绝对路径；不存在返回 false。POSIX realpath / Win GetFinalPathNameByHandle。 */
bool RealPath(const std::string& in, std::string* out);

bool IsDirectory(const std::string& p);
bool IsRegularFile(const std::string& p);

struct StatInfo {
  bool exists = false;
  bool is_dir = false;
  bool is_regular = false;
  int64_t size = 0;
  int mode = 0;  // POSIX 权限位；Windows 上是合成值（只读 = 0444）
  int64_t mtime_ms = 0;
};
bool Stat(const std::string& p, StatInfo* out);

/** 递归建目录，权限 0700（Windows 上靠继承 ACL，语义见实现注释）。 */
bool MakeDirs(const std::string& path, std::string* err);

/** 建一个只有本用户可进的临时目录。prefix 只是可读性前缀。 */
bool MakeTempDir(const std::string& prefix, std::string* out, std::string* err);

/** 删空目录。非空时失败 —— 故意的，留有痕迹便于排查。 */
bool RemoveDir(const std::string& path);

/** 用户主目录。rollout 默认落在这下面。 */
std::string HomeDir();

bool GetEnv(const char* name, std::string* out);

/** 目录项枚举。失败返回 false（调用方一律跳过，glob 不因一个不可读目录而整个失败）。 */
bool ListDir(const std::string& dir, std::vector<std::string>* names);

/** 单个路径分量的通配匹配（POSIX fnmatch(FNM_PATHNAME) 的等价物）。 */
bool MatchComponent(const std::string& pattern, const std::string& name);

// ---- 文件 ----

/** 读整个文件。 */
bool ReadWholeFile(const std::string& path, std::string* out, std::string* err);

/**
 * 原子替换：同目录临时文件 -> 落盘 -> rename。
 * 崩在中途要么是旧内容要么是新内容，不会出现半个文件。
 */
bool WriteFileAtomic(const std::string& path, const std::string& content, std::string* err);

bool Unlink(const std::string& path);

/** 按 UTF-8 路径打开 FILE*（Windows 上走宽字符，否则中文路径会静默打不开）。 */
std::FILE* FopenUtf8(const std::string& path, const char* mode);

/**
 * append-only 句柄。rollout 的持久性承诺全压在它身上：
 *   · 每条记录一次写 -> 进程被强杀也不会留下半行
 *   · 掉电级持久性靠 Sync()
 *
 * ★ 两个平台的"原子追加"来路不同，但保证一样：
 *     POSIX   O_APPEND：偏移与写在内核里是一次操作
 *     Windows FILE_APPEND_DATA 且不带 FILE_WRITE_DATA：同样由内核保证
 *   都只对「不超过管道/扇区缓冲的单次写」成立，所以短写必须当失败上报，
 *   不能假装成功 —— 日志是这个系统的事实来源，宁可报错也不能留半行。
 */
class AppendFile {
 public:
  AppendFile() = default;
  ~AppendFile();
  AppendFile(const AppendFile&) = delete;
  AppendFile& operator=(const AppendFile&) = delete;

  bool Open(const std::string& path, std::string* err);
  bool WriteRecord(const std::string& s);
  bool Sync();
  void Close();
  bool valid() const { return h_ != -1; }

 private:
  std::intptr_t h_ = -1;  // POSIX: fd。Windows: HANDLE
};

/** 本地日期，形如 "2026/09/11"（rollout 的目录结构）。 */
std::string LocalDatePath();

/** 挂钟毫秒（记录时间戳用；deadline 一律用 NowMs）。 */
int64_t UnixNowMs();

// ---- 路径形状 ----
//
// ★ Windows 的路径不是「/ 分隔的字符串」：有盘符、有 UNC、大小写不敏感。
//   path_guard 是整个系统里唯一把字符串变成路径的地方，它必须能问清楚
//   这些问题，否则 roots 包含判断在 Windows 上就是错的。

/** 首选分隔符（'/' 或 '\'）。 */
char PreferredSeparator();

/** 是否是分隔符（Windows 上 '/' 与 '\' 都算）。 */
bool IsSeparator(char c);

/** 绝对路径？Windows: "C:\x" 或 "\\server\share"；POSIX: 以 '/' 开头。 */
bool IsAbsolute(const std::string& p);

/** 把 rel 接到 base 后面（rel 为绝对路径时直接返回 rel）。 */
std::string Join(const std::string& base, const std::string& rel);

/** 规范化分隔符：Windows 上统一成 '\'，POSIX 上不变。 */
std::string Normalize(const std::string& p);

/** 取父目录。没有父目录时返回根。 */
std::string Parent(const std::string& p);

/** 路径相等/前缀比较用的折叠形式（Windows 大小写不敏感 → 转小写）。 */
std::string FoldCase(const std::string& p);

/** path 是否等于 root 或在 root 之下。两边都必须是已规范化的绝对路径。 */
bool IsWithin(const std::string& path, const std::string& root);

}  // namespace hx::platform
