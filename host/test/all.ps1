# The full regression suite (Windows). Matches test/all.sh one for one.
#
#   pwsh test\all.ps1
#
# ★ The four engine suites (escape / kill9 / pty / forkbomb) are written in
#   bash + python3 and have no direct counterpart on Windows: they probe the
#   behaviour of Landlock / seccomp / RLIMIT. The equivalent verification moved
#   to engine\tests\windows\smoke.ps1 -- it checks the same properties (what
#   should pass gets through, what should be refused is refused, the process
#   tree does not leak), just by asking AppContainer and Job objects instead.
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

Section "typecheck"
npx tsc --noEmit; Report $LASTEXITCODE "typecheck"

Section "architectural constraints"
pwsh -NoProfile -File test\arch.ps1 | Out-Null; Report $LASTEXITCODE "host must not touch fs / start processes"

Section "engine: sandbox / process tree / pty (Windows smoke)"
pwsh -NoProfile -File ..\engine\tests\windows\smoke.ps1 -Engine $HXD | Select-Object -Last 1
Report $LASTEXITCODE "AppContainer + Job objects + ConPTY"

Section "end to end: a multi-step task"
$WS = New-Workspace "e2e"
# Fixtures vary by platform; see test\fixtures.ts, which explains why only
# powershell works on Windows
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

Section "context compaction"
$WS = New-Workspace "compact"
npx tsx test\compaction.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "compaction"

Section "policy and approval"
$WS = New-Workspace "appr"; New-Item -ItemType Directory -Force -Path (Join-Path $WS "build") | Out-Null
npx tsx test\approval.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "approval"

Section "subagents and permission narrowing"
$WS = New-Workspace "sub"; Set-Content -Path (Join-Path $WS "f.txt") -Value "data" -Encoding utf8
npx tsx test\subagent.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "subagent"

Section "HTTP / SSE service"
$WS = New-Workspace "srv"; New-Item -ItemType Directory -Force -Path (Join-Path $WS "build") | Out-Null
npx tsx test\server.ts $WS $HXD | Select-Object -Last 2; Report $LASTEXITCODE "server"

Write-Host ""
if ($script:fail -eq 0) { Write-Host "all passed" -ForegroundColor Green } else { Write-Host "failures present" -ForegroundColor Red }
exit $script:fail
