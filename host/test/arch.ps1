# 架构约束的强制点（Windows 版，与 test/arch.sh 逐条对应）。
#
# host 决定"该不该做"，hxd 决定"能不能做"。如果 host 能直接碰文件系统或起进程，
# 那条边界就只是口头约定。
Set-Location (Join-Path $PSScriptRoot "..")

$EXEMPT = "src\engine\client.ts"   # 唯一豁免：它负责把引擎拉起来
$script:fail = 0

function Scan {
  param([string]$Pattern, [string]$What)
  $hits = Get-ChildItem -Recurse -Path src -Filter *.ts |
          Select-String -Pattern $Pattern |
          Where-Object { $_.Path -notlike "*$EXEMPT" }
  if ($hits) {
    Write-Host "  FAIL  host 不得$What：" -ForegroundColor Red
    $hits | ForEach-Object { Write-Host ("        " + $_.Path + ":" + $_.LineNumber + ": " + $_.Line.Trim()) }
    $script:fail = 1
  } else {
    Write-Host "  PASS  host 不$What" -ForegroundColor Green
  }
}

Write-Host "架构约束检查"
Scan 'from "(node:)?fs(/promises)?"' "直接碰文件系统"
Scan 'from "(node:)?child_process"' "起进程"
Scan 'from "(node:)?(net|dgram)"' "直接开网络连接"

# 豁免文件必须明确标注理由，防止豁免被无声扩大
if (Select-String -Path $EXEMPT -Pattern "eslint-disable no-restricted-imports" -Quiet) {
  Write-Host "  PASS  豁免文件 $EXEMPT 有显式标注" -ForegroundColor Green
} else {
  Write-Host "  FAIL  豁免文件缺少显式标注" -ForegroundColor Red
  $script:fail = 1
}

exit $script:fail
