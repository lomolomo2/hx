// Landlock 是 Linux 上 Confinement 的实现。
//
// 类型与契约都搬到了 sandbox/confine.hpp（跨平台）与 sandbox/confine_posix.hpp
// （Linux 私有）。这个头文件只剩转发，免得所有 POSIX 侧的 include 都要改。
#pragma once

#include "sandbox/confine.hpp"
#ifndef _WIN32
#include "sandbox/confine_posix.hpp"
#endif
