cmake_minimum_required(VERSION 3.20)
project(hx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# 平台无关的部分。
#
# ★ 划分原则：这里只放「决定做什么」的代码。
#   凡是要问操作系统「怎么做」的，都在下面按平台挑 —— 而且是整个文件挑，
#   不是在文件里撒 #ifdef。散落的 #ifdef 会让两条路径互相看不见，
#   改一边忘一边；整文件切分至少保证两边的函数签名必须对得上，编译器会盯着。
set(HX_COMMON_SOURCES
  src/main.cpp
  src/proto.cpp
  src/engine.cpp
  src/exec/cell.cpp
  src/fs/path_guard.cpp
  src/fs/apply_patch.cpp
  src/log/rollout.cpp
  src/sandbox/policy.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HX_PLATFORM_SOURCES
    src/platform/platform_posix.cpp
    src/exec/env_posix.cpp
    src/io/io_posix.cpp
    src/exec/proc_posix.cpp
    src/exec/spawn_posix.cpp
    src/exec/pty_posix.cpp
    src/exec/rlimit_posix.cpp
    src/sandbox/caps_posix.cpp
    src/sandbox/landlock.cpp
    src/sandbox/seccomp.cpp
    src/sandbox/cgroup.cpp
  )
elseif(WIN32)
  set(HX_PLATFORM_SOURCES
    src/platform/platform_win.cpp
    src/exec/env_win.cpp
    src/platform/platform_win_file.cpp
    src/platform/win_util.cpp
    src/io/io_win.cpp
    src/exec/proc_win.cpp
    src/exec/spawn_win.cpp
    src/exec/pty_win.cpp
    src/exec/rlimit_win.cpp
    src/sandbox/caps_win.cpp
    src/sandbox/confine_win.cpp
  )
else()
  message(FATAL_ERROR
    "hxd targets Linux (Landlock/seccomp/cgroup v2) and Windows (AppContainer/Job objects). "
    "macOS Seatbelt has a seam at sandbox/confine.hpp but no implementation.")
endif()

add_executable(hxd ${HX_COMMON_SOURCES} ${HX_PLATFORM_SOURCES})

target_include_directories(hxd PRIVATE src)
# vendor 用 SYSTEM，第三方头文件的告警不参与 -Werror
target_include_directories(hxd SYSTEM PRIVATE vendor)

if(WIN32)
  target_link_libraries(hxd PRIVATE
    userenv    # CreateAppContainerProfile / DeriveAppContainerSid
    advapi32   # SetEntriesInAcl / SetNamedSecurityInfo / CreateWellKnownSid
    kernel32   # Job 对象、ConPTY、IOCP
  )
  target_compile_definitions(hxd PRIVATE
    # Win10 起的 API：AppContainer 属性、ConPTY
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NOMINMAX               # windows.h 的 min/max 宏会和 std:: 打架
    WIN32_LEAN_AND_MEAN    # 少拖进来一堆 winsock1 之类的老东西
    UNICODE _UNICODE       # 只用 W 系列 API，见 platform/win_util.hpp
    _CRT_SECURE_NO_WARNINGS
  )
  if(MSVC)
    target_compile_options(hxd PRIVATE
      /W4 /WX
      /utf-8               # 源码是 UTF-8；不给这个 MSVC 会按本地代码页读注释
      /Zc:__cplusplus      # 否则 __cplusplus 永远报 199711
      /permissive-
      /EHsc
      # C4100: 未使用的形参 —— seam 两侧签名必须一致，某一边用不上是正常的
      /wd4100
    )
  endif()
  # tsx / node 起引擎时不该弹控制台窗口，但 hxd 本身是控制台程序（要 stdio）
  set_target_properties(hxd PROPERTIES WIN32_EXECUTABLE FALSE)
else()
  target_link_libraries(hxd PRIVATE util)  # forkpty
  target_compile_options(hxd PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(HX_SANITIZE "Build with ASan+UBSan (debug only)" OFF)
if(HX_SANITIZE AND NOT MSVC)
  target_compile_options(hxd PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(hxd PRIVATE -fsanitize=address,undefined)
endif()
