cmake_minimum_required(VERSION 3.20)
project(hx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The platform-independent part.
#
# ★ The dividing principle: only code that decides *what to do* lives here.
#   Anything that has to ask the operating system *how* is selected by platform
#   below -- and selected a whole file at a time, not by scattering #ifdefs
#   inside files. Scattered #ifdefs let the two paths drift out of sight of
#   each other, so one gets changed and the other forgotten; splitting by file
#   at least forces the signatures on both sides to line up, with the compiler
#   watching.
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
# vendor is SYSTEM so third-party header warnings do not feed into -Werror
target_include_directories(hxd SYSTEM PRIVATE vendor)

if(WIN32)
  target_link_libraries(hxd PRIVATE
    userenv    # CreateAppContainerProfile / DeriveAppContainerSid
    advapi32   # SetEntriesInAcl / SetNamedSecurityInfo / CreateWellKnownSid
    kernel32   # Job objects, ConPTY, IOCP
  )
  target_compile_definitions(hxd PRIVATE
    # APIs from Win10 on: the AppContainer attribute, ConPTY
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NOMINMAX               # windows.h's min/max macros fight with std::
    WIN32_LEAN_AND_MEAN    # drags in less legacy baggage such as winsock1
    UNICODE _UNICODE       # W APIs only; see platform/win_util.hpp
    _CRT_SECURE_NO_WARNINGS
  )
  if(MSVC)
    target_compile_options(hxd PRIVATE
      /W4 /WX
      /utf-8               # the sources are UTF-8; without this MSVC reads them in the local code page
      /Zc:__cplusplus      # otherwise __cplusplus always reports 199711
      /permissive-
      /EHsc
      # C4100: unused parameter -- both sides of a seam must share a signature,
      # so one side not using a parameter is normal
      /wd4100
    )
  endif()
  # Starting the engine from tsx / node should pop up no console window, but
  # hxd is itself a console program (it needs stdio)
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
