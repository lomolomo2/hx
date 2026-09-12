# hxd's Windows smoke suite -- the counterpart of engine/tests/escape/run.sh.
#
#   pwsh engine\tests\windows\smoke.ps1 [-Engine <path>] [-Workspace <dir>]
#
# It copies escape/run.sh's principle: verify both that what should be refused
# is refused, and that what should pass gets through. The README records this
# lesson -- granting /dev/null with the wrong access bits, and hardcoding
# RLIMIT_NPROC to 256: both times the escape suite was green while the actual
# work was entirely broken.
#
# How each section maps to the Linux side:
#   A usability    <- escape/run.sh's "usability" section
#   B file isolation <- Landlock allow-list           -> AppContainer + ACEs
#   C network      <- Landlock(TCP) + seccomp(UDP)    -> AppContainer capabilities (one layer covers both)
#   D process tree <- setsid + kill(-pgid) + sweeping -> Job objects (atomic, no sweeping)
#   E path_guard   <- the same code, run on both
#   F apply_patch  <- likewise
#   G pty         <- forkpty                        -> ConPTY
param(
  [string]$Engine = "",
  [string]$Workspace = ""
)

$ErrorActionPreference = "Continue"

if (-not $Engine) { $Engine = Join-Path $PSScriptRoot "..\..\build\hxd.exe" }
$exe = (Resolve-Path $Engine).Path

if (-not $Workspace) {
  $Workspace = Join-Path $env:TEMP ("hx-smoke-" + [System.IO.Path]::GetRandomFileName().Substring(0, 8))
}
New-Item -ItemType Directory -Force -Path $Workspace | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Workspace "sub") | Out-Null
Set-Content -Path (Join-Path $Workspace "note.txt") -Encoding utf8 -Value @("hello from the workspace", "second line")
Set-Content -Path (Join-Path $Workspace "sub\deep.txt") -Encoding utf8 -Value "nested"
$ws = (Resolve-Path $Workspace).Path

function Run-Hx {
  param([string[]]$Lines)
  $f = Join-Path $env:TEMP "hx-smoke-in.jsonl"
  Set-Content -Path $f -Value $Lines -Encoding utf8
  return (Get-Content $f | & $exe 2>&1)
}

function Open-Session {
  param([string]$Sandbox = "workspace-write", [string]$Net = "deny", [string]$Name = "smoke")
  $j = $ws -replace '\\', '\\\\'
  return '{"id":"open","op":"session.open","args":{"roots":["' + $j + '"],"sandbox":"' + $Sandbox + '","net":"' + $Net + '","name":"' + $Name + '"}}'
}

function Exec-Lines {
  param([string]$CmdJsonArray, [int]$TimeoutMs = 15000, [int]$Waits = 8)
  $l = @('{"id":"e","op":"exec.start","args":{"cmd":' + $CmdJsonArray + ',"timeout_ms":' + $TimeoutMs + '}}')
  for ($i = 0; $i -lt $Waits; $i++) {
    $l += '{"id":"w' + $i + '","op":"exec.wait","args":{"cell":"c1","yield_ms":1500,"max_bytes":65536}}'
  }
  return $l
}

$script:pass = 0
$script:fail = 0
function Check {
  param([string]$Name, [bool]$Ok, [string]$Detail = "")
  if ($Ok) { $script:pass++; Write-Host ("  PASS  " + $Name) -ForegroundColor Green }
  else { $script:fail++; Write-Host ("  FAIL  " + $Name + "  " + $Detail) -ForegroundColor Red }
}

Write-Host "=== A. usability: what should pass must get through ==="

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo usable"]'))
Check "commands run under workspace-write" ("$o" -match "usable")

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","type note.txt"]'))
Check "a file inside the root can be read" ("$o" -match "hello from the workspace")

# ★ These three were added later, and they close exactly the hole this suite
#   once had.
#
#   Earlier this only tested echo / type / writing files, all green, while
#   `dir` failed **everywhere** inside the sandbox -- an AppContainer cannot
#   read the volume root, and cmd's dir queries volume information, so even
#   listing its own workspace reported "Access is denied". Measured with a real
#   model, all 20 of its steps went on "where are my files".
#
#   This is the README's lesson itself: a security test verifies that the
#   forbidden was refused, and cannot detect that the permitted was blocked.
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b"]'))
Check "the workspace directory can be listed (cmd dir)" ("$o" -match "note\.txt")

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b sub"]'))
Check "a subdirectory can be listed" ("$o" -match "deep\.txt")

# PowerShell has to mount a PSDrive before its location can move to the
# workspace (Set-Location with an absolute path gives Access is denied -- it
# needs the parent directory, and the ancestor chain is not granted). That is
# exactly what the host's shellCommand prepends on the pwsh branch, and this
# verifies it really works.
$psFix = 'New-PSDrive -Name hx -PSProvider FileSystem -Root ([Environment]::CurrentDirectory) -Scope Global | Out-Null; Set-Location hx:; Get-ChildItem -Name'
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines ('["powershell","-NoProfile","-Command",' + ($psFix | ConvertTo-Json) + ']') 25000 12))
Check "the workspace directory can be listed (PowerShell + PSDrive)" ("$o" -match "note\.txt")

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo written > out.tmp && type out.tmp"]'))
Check "a file inside the root can be written" ("$o" -match "written")

# The session-private tmp: without it, every tool that needs a temp file
# (compilers, bundlers) fails for no visible reason
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo tmp-ok > %TEMP%\\probe.txt && type %TEMP%\\probe.txt"]'))
Check "the session-private tmp can be written" ("$o" -match "tmp-ok")

# NUL is /dev/null's counterpart: writing to it is a no-op, not a security
# boundary. Without it, every command in a read-only session gets polluted by
# "Access is denied" noise.
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo nul-ok > NUL && echo done"]'))
Check "the NUL device can be written" ("$o" -match "done")

Write-Host ""
Write-Host "=== B. file isolation: what should be refused must be refused ==="

# This corresponds to `cat ~/.ssh/id_rsa` returning EACCES in the README: no
# blacklist is involved, the user profile simply was never granted to the
# AppContainer SID.
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","type %USERPROFILE%\\.ssh\\id_rsa 2>&1"]'))
Check "a private key under the user profile cannot be read" (-not ("$o" -match "PRIVATE KEY"))

# ★ For `dir` to work, the volume root needs one minimal ACE (traverse +
#   read-attributes only, **not** list-contents, and not inherited). That is a
#   one-time machine setup, not something the engine stamps on every run -- see
#   CheckVolumeRoots in confine_win.cpp.
#   These two keep watch that the opening does not widen: neither the volume
#   root nor the user directory may be listable.
#   Note that matching on "Access is denied" alone is not enough -- dir used to
#   report that for every directory, so assertions of this kind would go
#   **green precisely because everything was broken**. What must be verified is
#   that the real contents are not visible.
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b C:\\"]'))
Check "the contents of volume root C:\ cannot be listed" (-not ("$o" -match "(?m)^\s*(Windows|Program Files|Users)\s*$"))

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b C:\\Users"]'))
Check "C:\Users cannot be listed" (-not ("$o" -match "(?m)^\s*(Public|Default)\s*$"))

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo nope > C:\\Windows\\hx-escape.txt && echo WROTE"]'))
Check "C:\Windows cannot be written" (-not ("$o" -match "WROTE"))

$o = Run-Hx -Lines (@(Open-Session -Sandbox "read-only") + (Exec-Lines '["cmd","/c","echo nope > ro.tmp && echo WROTE"]'))
Check "the root cannot be written under read-only" (-not ("$o" -match "WROTE"))

Write-Host ""
Write-Host "=== C. network: net=deny is kernel-enforced ==="

$o = Run-Hx -Lines (@(Open-Session -Net "deny") + (Exec-Lines '["cmd","/c","ping -n 1 -w 3000 8.8.8.8"]' 20000 12))
# ★ The non-English alternative below is deliberate and must not be "cleaned
#   up": ping localizes its output, so on a zh-CN Windows a successful reply
#   reads "来自 8.8.8.8" rather than "Reply from 8.8.8.8". Matching only the
#   English form would make this assertion pass on a localized machine even
#   when the network was NOT blocked -- a security test going green for the
#   wrong reason.
Check "net=deny blocks outbound traffic" (-not ("$o" -match "Reply from 8\.8\.8\.8|来自 8\.8\.8\.8"))

# ★ On Linux this only holds because seccomp blocks AF_INET (Landlock covers
#   TCP only, with UDP/DNS its blind spot). Windows needs no second layer:
#   without the internetClient capability, WFP blocks TCP and UDP alike in the
#   kernel.
$o = Run-Hx -Lines (@(Open-Session -Net "deny") + (Exec-Lines '["cmd","/c","nslookup example.com 8.8.8.8"]' 20000 12))
Check "net=deny blocks UDP/DNS (Landlock's blind spot)" (-not ("$o" -match "Address:\s*93\.|Non-authoritative"))

Write-Host ""
Write-Host "=== D. process tree ownership ==="

# orphans_killed has to be a field that carries information. GroupAlive used
# to count ActiveProcesses, and the result was true for every ordinary command
# (the direct child is still on the list, plus a conhost), which made the
# signal worthless. See exec/proc_win.cpp and DETACHED_PROCESS in
# spawn_win.cpp.
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo plain"]'))
$orph = ([regex]'"orphans_killed":(true|false)').Match("$o").Groups[1].Value
Check "orphans_killed=false when there is no background process" ($orph -eq "false") "got=$orph"

# The README's trap: the top-level shell of `cmd &` exits normally and
# immediately, leaving the background process on the system forever. start /b
# is its Windows equivalent.
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","start /b cmd /c ping -n 30 127.0.0.1 > NUL & echo spawned"]'))
$orph = ([regex]'"orphans_killed":(true|false)').Match("$o").Groups[1].Value
Check "a background process from start /b is collected" ($orph -eq "true") "got=$orph"

# ping cannot be used to test timeouts: under net=deny it reports "Unable to
# contact IP driver" and exits immediately, so the timeout branch is never
# reached. cmd /c pause waits on stdin forever, which really does hang.
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","pause"]' 2000 10))
Check "a timeout kills the process" ("$o" -match '"timed_out":true')

Write-Host ""
Write-Host "=== E. path_guard (the engine is not itself sandboxed, so everything rests on it) ==="

$o = Run-Hx -Lines @((Open-Session), '{"id":"r","op":"fs.read","args":{"path":"../../../../../../Windows/win.ini"}}')
Check "fs.read blocks a .. escape" ("$o" -match "E_PATH_ESCAPE")

$o = Run-Hx -Lines @((Open-Session), '{"id":"r","op":"fs.read","args":{"path":"C:\\Windows\\win.ini"}}')
Check "fs.read blocks an absolute-path escape" ("$o" -match "E_PATH_ESCAPE")

Write-Host ""
Write-Host "=== F. apply_patch ==="

$patch = '*** Begin Patch\n*** Add File: made.txt\n+line one\n+line two\n*** End Patch'
$patchLine = '{"id":"p","op":"fs.apply_patch","args":{"patch":"' + $patch + '"}}'
$readLine = '{"id":"r","op":"fs.read","args":{"path":"made.txt"}}'
$o = Run-Hx -Lines @((Open-Session), $patchLine, $readLine)
Check "apply_patch creates a file and it reads back" ("$o" -match "line one")

Write-Host ""
Write-Host "=== G. pty (ConPTY) ==="

$l = @((Open-Session),
  '{"id":"e","op":"exec.start","args":{"cmd":["cmd","/c","echo pty-works"],"pty":true,"rows":24,"cols":80,"timeout_ms":15000}}')
for ($i = 0; $i -lt 8; $i++) { $l += '{"id":"w' + $i + '","op":"exec.wait","args":{"cell":"c1","yield_ms":1500}}' }
$o = Run-Hx -Lines $l
Check "ConPTY runs a command" ("$o" -match "pty-works")

Write-Host ""
Write-Host "=== H. hygiene: residue from a hard kill is cleaned up by the next start ==="

# ★ This section was added to close the hardest failure of all to notice:
#   "the cleanup code ran and cleaned nothing up".
#
#   The first version of SweepStaleProfiles scanned %LOCALAPPDATA%\Packages,
#   while CreateAppContainerProfile only guarantees registering a moniker under
#   the registry's Mappings; that directory is created only on demand.
#   Measured across seven consecutive sessions: not one directory under
#   Packages, and all seven entries in the registry -- the sweeper might as
#   well not have been written, with nothing to indicate it.
function Get-HxProfiles {
  $k = "HKCU:\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppContainer\Mappings"
  return @(Get-ChildItem $k -ErrorAction SilentlyContinue |
    ForEach-Object { (Get-ItemProperty $_.PSPath).Moniker } |
    Where-Object { $_ -like "hx-*" })
}

$before = Get-HxProfiles

# Open a session and then **hard-kill** the engine: the destructor does not
# run, so a profile is certain to be left behind.
$killFile = Join-Path $env:TEMP "hx-smoke-kill.jsonl"
$j = $ws -replace '\\', '\\\\'
Set-Content -Path $killFile -Encoding utf8 -Value @(
  (Open-Session),
  '{"id":"e","op":"exec.start","args":{"cmd":["cmd","/c","pause"],"timeout_ms":60000}}',
  '{"id":"w","op":"exec.wait","args":{"cell":"c1","yield_ms":20000}}')
$victim = Start-Process $exe -PassThru -NoNewWindow -RedirectStandardInput $killFile `
  -RedirectStandardOutput (Join-Path $env:TEMP "hx-smoke-kill.out") `
  -RedirectStandardError (Join-Path $env:TEMP "hx-smoke-kill.err")
Start-Sleep -Milliseconds 1200
Stop-Process -Id $victim.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

$leaked = @(Get-HxProfiles | Where-Object { $before -notcontains $_ })
Check "the hard kill really did leave a profile (otherwise this test is empty)" ($leaked.Count -gt 0) "leaked=$($leaked.Count)"

# Start another engine: its startup sweep should collect the one above.
Run-Hx -Lines @((Open-Session)) | Out-Null
Start-Sleep -Milliseconds 400
$after = Get-HxProfiles
$still = @($leaked | Where-Object { $after -contains $_ })
Check "the next start cleaned the residue up" ($still.Count -eq 0) "still=$($still -join ',')"

Write-Host ""
Write-Host "pass=$($script:pass) fail=$($script:fail)"
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $ws "out.tmp"), (Join-Path $ws "made.txt"), (Join-Path $ws "ro.tmp")
exit $script:fail
