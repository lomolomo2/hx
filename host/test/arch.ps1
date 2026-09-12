# Where the architectural constraint is enforced (Windows version, matching
# test/arch.sh line for line).
#
# The host decides "should this be done", hxd decides "can this be done". If the
# host could touch the filesystem or start processes directly, that boundary
# would be nothing but a verbal agreement.
Set-Location (Join-Path $PSScriptRoot "..")

$EXEMPT = "src\engine\client.ts"   # the sole exemption: it brings the engine up
$script:fail = 0

function Scan {
  param([string]$Pattern, [string]$What)
  $hits = Get-ChildItem -Recurse -Path src -Filter *.ts |
          Select-String -Pattern $Pattern |
          Where-Object { $_.Path -notlike "*$EXEMPT" }
  if ($hits) {
    Write-Host "  FAIL  host must not $What`:" -ForegroundColor Red
    $hits | ForEach-Object { Write-Host ("        " + $_.Path + ":" + $_.LineNumber + ": " + $_.Line.Trim()) }
    $script:fail = 1
  } else {
    Write-Host "  PASS  host does not $What" -ForegroundColor Green
  }
}

Write-Host "architectural constraint check"
Scan 'from "(node:)?fs(/promises)?"' "touch the filesystem directly"
Scan 'from "(node:)?child_process"' "start processes"
Scan 'from "(node:)?(net|dgram)"' "open network connections directly"

# The exempt file must state its reason explicitly, so the exemption cannot be
# widened silently
if (Select-String -Path $EXEMPT -Pattern "eslint-disable no-restricted-imports" -Quiet) {
  Write-Host "  PASS  exempt file $EXEMPT carries an explicit marker" -ForegroundColor Green
} else {
  Write-Host "  FAIL  exempt file is missing its explicit marker" -ForegroundColor Red
  $script:fail = 1
}

exit $script:fail
