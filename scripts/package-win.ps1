# Build a Windows executable package.
#
#   pwsh scripts\package-win.ps1 -Version v0.2.0
#
# Output: dist\hx-windows-x64-<version>.zip -- unpack and it works, with no
# need to clone the repository.
#
# What is in the package:
#   hx.cmd / hx.ps1     the launchers (the same files, which recognise for
#                       themselves whether this is a release or a working tree)
#   hxd.exe             the engine: a native binary with no runtime dependencies
#   host\hx-host.mjs    the host, bundled to a single file by esbuild
#   README.md LICENSE   how to install and use it
#
# ★ Why the host is not made into an .exe: it is JS, and turning it into a
#   standalone binary means embedding a whole Node runtime (40MB and up, plus a
#   re-release every time Node ships a security fix). The engine is the half
#   that has to be native -- it is what builds the sandbox. The host depends on
#   a system Node 20+, and that is stated plainly in the README and in the
#   launcher rather than left to fail mysteriously at runtime.
param(
  [string]$Version = "",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
  $Version = (git -C $repo describe --tags --always 2>$null)
  if (-not $Version) { $Version = "dev" }
}

$staging = Join-Path $repo "dist\hx-windows-x64-$Version"
$zip = Join-Path $repo "dist\hx-windows-x64-$Version.zip"

Write-Host "version : $Version"
Write-Host "staging : $staging"
Write-Host ""

# ---------------------------------------------------------------- engine
if (-not $SkipBuild) {
  Write-Host "== building the engine ==" -ForegroundColor Cyan
  pwsh -NoProfile -File (Join-Path $repo "engine\build-win.ps1")
  if ($LASTEXITCODE -ne 0) { throw "engine build failed" }
}
$hxd = Join-Path $repo "engine\build\hxd.exe"
if (-not (Test-Path $hxd)) { throw "hxd.exe not found: $hxd" }

# ★ Whatever gets shipped must be verified first. --self-test exit code 0 =
#   this machine has a real sandbox; releasing a binary that cannot even pass
#   its own self-test pushes the burden of proving "there is a sandbox" onto
#   the user.
Write-Host ""
Write-Host "== self-test (must pass before release) ==" -ForegroundColor Cyan
& $hxd --self-test | Out-Null
if ($LASTEXITCODE -ne 0) { throw "hxd --self-test exited $LASTEXITCODE -- refusing to package" }
Write-Host "  self-test OK" -ForegroundColor Green

# ---------------------------------------------------------------- host
Write-Host ""
Write-Host "== bundling the host ==" -ForegroundColor Cyan
$esbuild = Join-Path $repo "host\node_modules\.bin\esbuild.cmd"
if (-not (Test-Path $esbuild)) { throw "esbuild is missing: run npm install once in $repo\host" }

if (Test-Path $staging) { [IO.Directory]::Delete($staging, $true) }
New-Item -ItemType Directory -Force -Path (Join-Path $staging "host") | Out-Null

# --packages=bundle pulls hono/zod in too, so no npm install is needed after
# unpacking. --external:node:* leaves Node's own built-ins alone.
# ★ `--outfile=(Join-Path ...)` gets split by PowerShell into **two**
#   arguments, so esbuild sees two input files and reports 'Must use "outdir"
#   when there are multiple input files' -- entirely unrelated to the real
#   problem. Build the string first, then pass it.
$entry = Join-Path $repo "host\src\cli.ts"
$outfile = Join-Path $staging "host\hx-host.mjs"
& $esbuild $entry --bundle --platform=node --target=node20 --format=esm `
  "--outfile=$outfile" --external:node:* --log-level=warning
if ($LASTEXITCODE -ne 0) { throw "esbuild failed" }

# ---------------------------------------------------------------- assemble
Copy-Item $hxd (Join-Path $staging "hxd.exe")
Copy-Item (Join-Path $repo "hx.cmd") $staging
Copy-Item (Join-Path $repo "hx.ps1") $staging
Copy-Item (Join-Path $repo "LICENSE") $staging
Copy-Item (Join-Path $repo "docs\windows.md") (Join-Path $staging "README.md")

Write-Host ""
Write-Host "== package contents ==" -ForegroundColor Cyan
Get-ChildItem $staging -Recurse -File | ForEach-Object {
  $rel = $_.FullName.Substring($staging.Length + 1)
  Write-Host ("  {0,-24} {1,10:N0} bytes" -f $rel, $_.Length)
}

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip
$size = (Get-Item $zip).Length

Write-Host ""
Write-Host ("zip     : $zip  ({0:N1} MB)" -f ($size / 1MB)) -ForegroundColor Green
Write-Host ("sha256  : " + (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower())
