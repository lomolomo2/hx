# 在 Windows 上构建 hxd.exe。
#
#   pwsh engine\build-win.ps1              # 增量构建（首次会自动 configure）
#   pwsh engine\build-win.ps1 -Configure   # 强制重新 configure
#   pwsh engine\build-win.ps1 -Clean       # 删掉 build 目录重来
#
# ★ 为什么需要这个脚本，而不是直接 `cmake -S . -B build`：
#   ① MSVC 的 cl.exe 只有在开发者环境里才在 PATH 上 —— 必须先过 vcvars64.bat。
#   ② 系统上装的 cmake 可能低于 CMakeLists 要求的 3.20（实测 3.17）。
#      Visual Studio 自带了一份够新的 cmake 和 ninja，这里直接用它们，
#      省得为了构建再去装一套工具链。
param(
  [switch]$Configure,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"
$engineDir = $PSScriptRoot
$buildDir = Join-Path $engineDir "build"

# ---- 找 Visual Studio ----
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
  throw "vswhere.exe not found. Install Visual Studio 2019 16.11+ with the C++ workload."
}
$vs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
if (-not $vs) {
  throw "No Visual Studio with the C++ toolset found (need the 'Desktop development with C++' workload)."
}

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmExt  = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake"
$cmake  = Join-Path $cmExt "CMake\bin\cmake.exe"
$ninja  = Join-Path $cmExt "Ninja\ninja.exe"

# VS 没带 cmake/ninja 时退回 PATH 上的（那就得自己保证版本够新）
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
if (-not (Test-Path $ninja)) { $ninja = (Get-Command ninja -ErrorAction Stop).Source }
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
$needConfigure = $Configure -or $Clean -or -not (Test-Path (Join-Path $buildDir "build.ninja"))

Write-Host "VS     : $vs"
Write-Host "cmake  : $cmake"
Write-Host "build  : $buildDir"
Write-Host ""

# vcvars64.bat 只影响它自己那个 cmd 进程，所以配置和构建要串在同一条命令里。
$steps = @()
if ($needConfigure) {
  $steps += "`"$cmake`" -S `"$engineDir`" -B `"$buildDir`" -G Ninja " +
            "-DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=RelWithDebInfo"
}
$steps += "`"$cmake`" --build `"$buildDir`""

$cmd = "`"$vcvars`" >nul 2>&1 && " + ($steps -join " && ")
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "built: $(Join-Path $buildDir 'hxd.exe')" -ForegroundColor Green
