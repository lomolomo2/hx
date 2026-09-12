// io_win.cpp 对外露出的 Windows 专用入口。
//
// 只有 spawn_win.cpp / pty_win.cpp 需要它：它们得拿到「给子进程的那一端」
// 的原始 HANDLE，才能填进 STARTUPINFO。除此之外没人该碰到 HANDLE。
#pragma once
#ifdef _WIN32

#include <windows.h>

#include <string>

#include "io/io.hpp"

namespace hx::io::win {

/**
 * 建一对管道：父端 overlapped 且已挂进 IOCP，子端同步且可继承。
 *
 * ★ 为什么不用 CreatePipe：它建出来的是匿名管道，不支持 overlapped I/O，
 *   于是父端只能阻塞读 —— 单线程引擎一读就冻住。标准做法是用带唯一名字的
 *   命名管道自己接一对，父端拿 FILE_FLAG_OVERLAPPED，子端拿同步句柄
 *   （子进程是普通程序，它假设 stdio 是同步的）。
 *
 * parent_reads = true  -> 父读子写（子进程的 stdout/stderr）
 * parent_reads = false -> 父写子读（子进程的 stdin）
 */
bool CreatePipePair(bool parent_reads, Fd* parent, HANDLE* child_end, std::string* err);

/**
 * 把一个已经存在的 overlapped HANDLE 纳入流表（ConPTY 的主端用）。
 * for_read / for_write 决定挂哪一侧；同一个 HANDLE 两侧都要时传两个 true。
 */
Fd AdoptHandle(HANDLE h, bool for_read, bool for_write, std::string* err);

/** 供诊断用；正常路径不该需要。 */
HANDLE RawHandle(Fd fd);

}  // namespace hx::io::win

#endif  // _WIN32
