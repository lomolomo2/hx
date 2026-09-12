# 全量回归（Windows）。与 test/all.sh 一一对应。
#
#   pwsh test\all.ps1
#
# ★ 引擎那四个套件（escape / kill9 / pty / forkbomb）是 bash + python3 写的，
#   在 Windows 上没有直接对位：它们探的是 Landlock / seccomp / RLIMIT 的行为。
#   对应的验证搬到了 engine\tests\windows\smoke.ps1 —— 验的是同一批性质
#   （该放的放得通、该拒的拒掉、进程树不外泄），只是问的是 AppContainer
#   和 Job 对象。
$ErrorActionPreference = "Continue"
Set-Location (Join-Path $PSScriptRoot "..")

$HXD = if ($env:HX_ENGINE) { $env:HX_ENGINE } else { (Resolve-Path "..\engine\build\hxd.exe" -ErrorAction SilentlyContinue) }
if (-not $HXD) { Write-Host "hxd.exe not found; build the engine first" -ForegroundColor Red; exit 1 }
$HXD = "$HXD"
$script:fail = 0

function Section { param($t) Write-Host ""; Write-Host "== $t ==" -ForegroundColor White }
function Report {
  param([int]$Code, [string]$Name)
  if ($Code -eq 0) { Write-Host "OK   $Name" -ForegroundColor Green }
  else { Write-Host "FAIL $Name" -ForegroundColor Red; $script:fail = 1 }
}

function New-Workspace {
  param([string]$Tag)
  $d = Join-Path $env:TEMP ("hx-t-" + $Tag + "-" + [System.IO.Path]::GetRandomFileName().Substring(0,8))
  New-Item -ItemType Directory -Force -Path $d | Out-Null
  return $d
}

Section "类型检查"
npx tsc --noEmit; Report $LASTEXITCODE "typecheck"

Section "架构约束"
pwsh -NoProfile -File test\arch.ps1 | Out-Null; Report $LASTEXITCODE "host 不得直接碰 fs / 起进程"

Section "引擎：沙箱 / 进程树 / pty（Windows 冒烟）"
pwsh -NoProfile -File ..\engine\tests\windows\smoke.ps1 -Engine $HXD | Select-Object -Last 1
Report $LASTEXITCODE "AppContainer + Job 对象 + ConPTY"

Section "端到端：多步任务"
$WS = New-Workspace "e2e"
# 夹具随平台变，见 test\fixtures.ts（那里写了为什么 Windows 上只能用 powershell）
Set-Content -Path (Join-Path $WS "calc.ps1") -Encoding utf8 -Value @(
  'function Add-Values($a, $b) {'
  '    return $a - $b'
  '}'
)
Set-Content -Path (Join-Path $WS "test_calc.ps1") -Encoding utf8 -Value @(
  "`$ErrorActionPreference = 'Stop'"
  '. "$PSScriptRoot\calc.ps1"'
  '$r = Add-Values 2 3'
  'if ($r -ne 5) { Write-Output "FAIL: got $r"; exit 1 }'
  "Write-Output 'ALL TESTS PASS'"
)
npx tsx test\e2e.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "e2e"

Section "上下文压缩"
$WS = New-Workspace "compact"
npx tsx test\compaction.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "compaction"

Section "策略与审批"
$WS = New-Workspace "appr"; New-Item -ItemType Directory -Force -Path (Join-Path $WS "build") | Out-Null
npx tsx test\approval.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "approval"

Section "子 agent 与权限收窄"
$WS = New-Workspace "sub"; Set-Content -Path (Join-Path $WS "f.txt") -Value "data" -Encoding utf8
npx tsx test\subagent.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "subagent"

Section "HTTP / SSE 服务"
$WS = New-Workspace "srv"; New-Item -ItemType Directory -Force -Path (Join-Path $WS "build") | Out-Null
npx tsx test\server.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "server"

Write-Host ""
if ($script:fail -eq 0) { Write-Host "全部通过" -ForegroundColor Green } else { Write-Host "有失败项" -ForegroundColor Red }
exit $script:fail
