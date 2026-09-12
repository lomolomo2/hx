# 打一个 Windows 可执行包。
#
#   pwsh scripts\package-win.ps1 -Version v0.2.0
#
# 产物：dist\hx-windows-x64-<version>.zip，解开就能用，不需要克隆仓库。
#
# 包里有什么：
#   hx.cmd / hx.ps1     启动器（同一份，自己认得出是发布包还是工作树）
#   hxd.exe             引擎，原生二进制，无运行时依赖
#   host\hx-host.mjs    宿主，esbuild 打成单文件
#   README.md LICENSE   装法与用法
#
# ★ 为什么宿主不做成 .exe：它是 JS，要变成独立二进制得塞进一整个 Node
#   运行时（40MB 起，而且每次 Node 出安全更新都得重新发一遍）。引擎才是
#   必须原生的那一半 —— 沙箱是它建的。宿主依赖系统装的 Node 20+，
#   这一条在 README 和启动器里都写清楚，而不是让它在运行时莫名其妙地失败。
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

# ---------------------------------------------------------------- 引擎
if (-not $SkipBuild) {
  Write-Host "== 构建引擎 ==" -ForegroundColor Cyan
  pwsh -NoProfile -File (Join-Path $repo "engine\build-win.ps1")
  if ($LASTEXITCODE -ne 0) { throw "engine build failed" }
}
$hxd = Join-Path $repo "engine\build\hxd.exe"
if (-not (Test-Path $hxd)) { throw "hxd.exe not found: $hxd" }

# ★ 发的东西必须自己先验一遍。--self-test 退出码 0 = 这台机器上有真沙箱；
#   拿一个连自检都过不去的二进制去发布，等于把"有沙箱"这句话的举证责任
#   推给了用户。
Write-Host ""
Write-Host "== 自检（发布前必须过）==" -ForegroundColor Cyan
& $hxd --self-test | Out-Null
if ($LASTEXITCODE -ne 0) { throw "hxd --self-test exited $LASTEXITCODE -- refusing to package" }
Write-Host "  self-test OK" -ForegroundColor Green

# ---------------------------------------------------------------- 宿主
Write-Host ""
Write-Host "== 打包宿主 ==" -ForegroundColor Cyan
$esbuild = Join-Path $repo "host\node_modules\.bin\esbuild.cmd"
if (-not (Test-Path $esbuild)) { throw "esbuild 不在：先在 $repo\host 里跑一次 npm install" }

if (Test-Path $staging) { [IO.Directory]::Delete($staging, $true) }
New-Item -ItemType Directory -Force -Path (Join-Path $staging "host") | Out-Null

# --packages=bundle 把 hono/zod 一起打进来，解包后不需要 npm install。
# --external:node:* 留给 Node 自己的内置模块。
# ★ `--outfile=(Join-Path ...)` 会被 PowerShell 拆成**两个**参数，
#   esbuild 于是看到两个输入文件，报 'Must use "outdir" when there are
#   multiple input files' —— 和真正的问题毫无关系。先拼成字符串再传。
$entry = Join-Path $repo "host\src\cli.ts"
$outfile = Join-Path $staging "host\hx-host.mjs"
& $esbuild $entry --bundle --platform=node --target=node20 --format=esm `
  "--outfile=$outfile" --external:node:* --log-level=warning
if ($LASTEXITCODE -ne 0) { throw "esbuild failed" }

# ---------------------------------------------------------------- 组装
Copy-Item $hxd (Join-Path $staging "hxd.exe")
Copy-Item (Join-Path $repo "hx.cmd") $staging
Copy-Item (Join-Path $repo "hx.ps1") $staging
Copy-Item (Join-Path $repo "LICENSE") $staging
Copy-Item (Join-Path $repo "docs\windows.md") (Join-Path $staging "README.md")

Write-Host ""
Write-Host "== 包内容 ==" -ForegroundColor Cyan
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
