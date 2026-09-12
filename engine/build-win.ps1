# Build hxd.exe on Windows.
#
#   pwsh engine\build-win.ps1              # incremental build (configures automatically the first time)
#   pwsh engine\build-win.ps1 -Configure   # force a reconfigure
#   pwsh engine\build-win.ps1 -Clean       # delete the build directory and start over
#
# ★ Why this script exists rather than just `cmake -S . -B build`:
#   1. MSVC's cl.exe is only on PATH inside a developer environment -- you have
#      to go through vcvars64.bat first.
#   2. The system's cmake may be older than the 3.20 CMakeLists requires
#      (measured: 3.17). Visual Studio ships a new enough cmake and ninja, so
#      this uses those directly and saves installing another toolchain just to
#      build.
param(
  [switch]$Configure,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"
$engineDir = $PSScriptRoot
$buildDir = Join-Path $engineDir "build"

# ---- locate Visual Studio ----
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

# If VS ships no cmake/ninja, fall back to whatever is on PATH (in which case
# the version is your responsibility)
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
if (-not (Test-Path $ninja)) { $ninja = (Get-Command ninja -ErrorAction Stop).Source }
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
$needConfigure = $Configure -or $Clean -or -not (Test-Path (Join-Path $buildDir "build.ninja"))

Write-Host "VS     : $vs"
Write-Host "cmake  : $cmake"
Write-Host "build  : $buildDir"
Write-Host ""

# vcvars64.bat only affects its own cmd process, so configure and build have to
# be chained into one command.
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
