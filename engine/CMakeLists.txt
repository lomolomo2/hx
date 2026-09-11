cmake_minimum_required(VERSION 3.20)
project(hx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "hxd currently targets Linux only (Landlock/seccomp/cgroup v2). See sandbox/backend seam.")
endif()

add_executable(hxd
  src/main.cpp
  src/proto.cpp
  src/engine.cpp
  src/exec/cell.cpp
  src/fs/path_guard.cpp
  src/fs/apply_patch.cpp
  src/log/rollout.cpp
  src/sandbox/caps.cpp
  src/sandbox/policy.cpp
  src/sandbox/landlock.cpp
  src/sandbox/seccomp.cpp
  src/sandbox/cgroup.cpp
  src/exec/spawn.cpp
  src/exec/env.cpp
  src/exec/rlimit.cpp
  src/exec/pty.cpp
)

target_link_libraries(hxd PRIVATE util)  # forkpty
target_include_directories(hxd PRIVATE src)
# vendor 用 SYSTEM，第三方头文件的告警不参与 -Werror
target_include_directories(hxd SYSTEM PRIVATE vendor)

target_compile_options(hxd PRIVATE -Wall -Wextra -Wpedantic -Werror)

option(HX_SANITIZE "Build with ASan+UBSan (debug only)" OFF)
if(HX_SANITIZE)
  target_compile_options(hxd PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(hxd PRIVATE -fsanitize=address,undefined)
endif()
