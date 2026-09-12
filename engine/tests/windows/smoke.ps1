# hxd 的 Windows 冒烟套件 —— engine/tests/escape/run.sh 的对位。
#
#   pwsh engine\tests\windows\smoke.ps1 [-Engine <path>] [-Workspace <dir>]
#
# 照搬 escape/run.sh 的那条原则：既验「该拒的拒了」，也验「该放的必须放得通」。
# README 记着这条教训 —— /dev/null 授权带错访问位、RLIMIT_NPROC 写死 256，
# 两次都是逃逸套件全绿而实际工作全废。
#
# 各节与 Linux 侧的对应：
#   A 可用性      <- escape/run.sh 的「可用性」一节
#   B 文件隔离    <- Landlock allow-list            -> AppContainer + ACE
#   C 网络        <- Landlock(TCP) + seccomp(UDP)   -> AppContainer 能力集（一层覆盖两者）
#   D 进程树      <- setsid + kill(-pgid) + 反复清扫 -> Job 对象（原子，无需清扫）
#   E path_guard  <- 同一份代码，两边都跑
#   F apply_patch <- 同上
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

Write-Host "=== A. 可用性：该放的必须放得通 ==="

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo usable"]'))
Check "workspace-write 下能跑命令" ("$o" -match "usable")

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","type note.txt"]'))
Check "能读 root 内的文件" ("$o" -match "hello from the workspace")

# ★ 这三条是补上来的，补的正是本套件曾经漏掉的那个洞。
#
#   早先这里只测了 echo / type / 写文件，全绿；而 `dir` 在沙箱里是**全线失败**的
#   —— AppContainer 读不了卷根，cmd 的 dir 要查卷信息，于是连列自己的工作区
#   都报 "Access is denied"。实测拿真模型跑任务时，它 20 步全耗在
#   "我的文件到底在哪"上。
#
#   这就是 README 那条教训本身：安全测试只验"该拒的拒了"，验不出"该放的没放"。
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b"]'))
Check "能列工作区目录（cmd dir）" ("$o" -match "note\.txt")

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b sub"]'))
Check "能列子目录" ("$o" -match "deep\.txt")

# PowerShell 要先挂 PSDrive 才能把 location 切到工作区（Set-Location 直接用绝对路径
# 会 Access is denied —— 它要访问父目录，而父链是不授权的）。宿主的 shellCommand
# 给 pwsh 分支加的就是这一段，这里验它确实管用。
$psFix = 'New-PSDrive -Name hx -PSProvider FileSystem -Root ([Environment]::CurrentDirectory) -Scope Global | Out-Null; Set-Location hx:; Get-ChildItem -Name'
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines ('["powershell","-NoProfile","-Command",' + ($psFix | ConvertTo-Json) + ']') 25000 12))
Check "能列工作区目录（PowerShell + PSDrive）" ("$o" -match "note\.txt")

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo written > out.tmp && type out.tmp"]'))
Check "能写 root 内的文件" ("$o" -match "written")

# 会话私有 tmp：不给它，凡是要落临时文件的工具（编译器、打包器）都会莫名其妙地失败
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo tmp-ok > %TEMP%\\probe.txt && type %TEMP%\\probe.txt"]'))
Check "能写会话私有 tmp" ("$o" -match "tmp-ok")

# NUL 是 /dev/null 的对位：写它是空操作，不是安全边界。
# 少了它，read-only 会话里每条命令都会被 "Access is denied" 噪音污染。
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo nul-ok > NUL && echo done"]'))
Check "能写 NUL 设备" ("$o" -match "done")

Write-Host ""
Write-Host "=== B. 文件隔离：该拒的必须拒掉 ==="

# 这一条对应 README 里 `cat ~/.ssh/id_rsa` 返回 EACCES：
# 不需要任何黑名单，用户 profile 只是没被授予 AppContainer SID 而已。
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","type %USERPROFILE%\\.ssh\\id_rsa 2>&1"]'))
Check "读不到用户 profile 下的私钥" (-not ("$o" -match "PRIVATE KEY"))

# ★ 为了让 `dir` 能用，卷根上需要一条最小 ACE（只给遍历 + 读属性，
#   **不给列内容**，不继承）。那是一次性的机器设置，不是引擎每次去打的
#   —— 见 confine_win.cpp 的 CheckVolumeRoots。
#   这两条盯着那个口子别变大：卷根和用户目录都必须列不出来。
#   注意不能只匹配 "Access is denied" —— 早先 dir 对任何目录都报这个，
#   于是这类断言会**因为全都坏掉而全绿**。要验的是"看不到真实内容"。
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b C:\\"]'))
Check "列不了卷根 C:\ 的内容" (-not ("$o" -match "(?m)^\s*(Windows|Program Files|Users)\s*$"))

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","dir /b C:\\Users"]'))
Check "列不了 C:\Users" (-not ("$o" -match "(?m)^\s*(Public|Default)\s*$"))

$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo nope > C:\\Windows\\hx-escape.txt && echo WROTE"]'))
Check "写不了 C:\Windows" (-not ("$o" -match "WROTE"))

$o = Run-Hx -Lines (@(Open-Session -Sandbox "read-only") + (Exec-Lines '["cmd","/c","echo nope > ro.tmp && echo WROTE"]'))
Check "read-only 下写不了 root" (-not ("$o" -match "WROTE"))

Write-Host ""
Write-Host "=== C. 网络：net=deny 是内核强制 ==="

$o = Run-Hx -Lines (@(Open-Session -Net "deny") + (Exec-Lines '["cmd","/c","ping -n 1 -w 3000 8.8.8.8"]' 20000 12))
Check "net=deny 挡住出站" (-not ("$o" -match "Reply from 8\.8\.8\.8|来自 8\.8\.8\.8"))

# ★ 这一条在 Linux 上要靠 seccomp 封 AF_INET 才成立（Landlock 只管 TCP，
#   UDP/DNS 是它的盲区）。Windows 这边不需要第二层：没有 internetClient 能力时
#   WFP 在内核里把 TCP 和 UDP 一起挡掉。
$o = Run-Hx -Lines (@(Open-Session -Net "deny") + (Exec-Lines '["cmd","/c","nslookup example.com 8.8.8.8"]' 20000 12))
Check "net=deny 挡住 UDP/DNS（Landlock 的盲区）" (-not ("$o" -match "Address:\s*93\.|Non-authoritative"))

Write-Host ""
Write-Host "=== D. 进程树所有权 ==="

# orphans_killed 必须是个有信号量的字段。早先 GroupAlive 用 ActiveProcesses 计数，
# 结果每条普通命令都报 true（直接子进程自己还在名单里，外加一个 conhost），
# 这个信号就废了。见 exec/proc_win.cpp 与 spawn_win.cpp 的 DETACHED_PROCESS。
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","echo plain"]'))
$orph = ([regex]'"orphans_killed":(true|false)').Match("$o").Groups[1].Value
Check "无后台进程时 orphans_killed=false" ($orph -eq "false") "got=$orph"

# README 的坑：`cmd &` 的顶层 shell 会立刻正常退出，后台进程永远留在系统里。
# start /b 是它在 Windows 上的等价物。
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","start /b cmd /c ping -n 30 127.0.0.1 > NUL & echo spawned"]'))
$orph = ([regex]'"orphans_killed":(true|false)').Match("$o").Groups[1].Value
Check "start /b 的后台进程被收掉" ($orph -eq "true") "got=$orph"

# 不能用 ping 验超时：net=deny 下它会立刻报 "Unable to contact IP driver" 而退出，
# 于是根本走不到超时分支。cmd /c pause 会一直等 stdin，才是真的挂住。
$o = Run-Hx -Lines (@(Open-Session) + (Exec-Lines '["cmd","/c","pause"]' 2000 10))
Check "超时会杀掉进程" ("$o" -match '"timed_out":true')

Write-Host ""
Write-Host "=== E. path_guard（引擎自己不在沙箱里，全靠它） ==="

$o = Run-Hx -Lines @((Open-Session), '{"id":"r","op":"fs.read","args":{"path":"../../../../../../Windows/win.ini"}}')
Check "fs.read 挡住 .. 逃逸" ("$o" -match "E_PATH_ESCAPE")

$o = Run-Hx -Lines @((Open-Session), '{"id":"r","op":"fs.read","args":{"path":"C:\\Windows\\win.ini"}}')
Check "fs.read 挡住绝对路径逃逸" ("$o" -match "E_PATH_ESCAPE")

Write-Host ""
Write-Host "=== F. apply_patch ==="

$patch = '*** Begin Patch\n*** Add File: made.txt\n+line one\n+line two\n*** End Patch'
$patchLine = '{"id":"p","op":"fs.apply_patch","args":{"patch":"' + $patch + '"}}'
$readLine = '{"id":"r","op":"fs.read","args":{"path":"made.txt"}}'
$o = Run-Hx -Lines @((Open-Session), $patchLine, $readLine)
Check "apply_patch 能建文件并读回" ("$o" -match "line one")

Write-Host ""
Write-Host "=== G. pty (ConPTY) ==="

$l = @((Open-Session),
  '{"id":"e","op":"exec.start","args":{"cmd":["cmd","/c","echo pty-works"],"pty":true,"rows":24,"cols":80,"timeout_ms":15000}}')
for ($i = 0; $i -lt 8; $i++) { $l += '{"id":"w' + $i + '","op":"exec.wait","args":{"cell":"c1","yield_ms":1500}}' }
$o = Run-Hx -Lines $l
Check "ConPTY 能跑命令" ("$o" -match "pty-works")

Write-Host ""
Write-Host "=== H. 卫生：被强杀之后的残留会被下一次启动清掉 ==="

# ★ 这一节是补的，补的是"清理代码跑了但什么都没清掉"这种最难发现的失败。
#
#   SweepStaleProfiles 第一版扫的是 %LOCALAPPDATA%\Packages，而
#   CreateAppContainerProfile 只保证在注册表 Mappings 下登记一条 moniker，
#   那个目录是按需才建的。实测连跑七个会话，Packages 下一个目录都没有、
#   注册表里七条全在 —— 清扫器等于没写，而且没有任何迹象。
function Get-HxProfiles {
  $k = "HKCU:\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppContainer\Mappings"
  return @(Get-ChildItem $k -ErrorAction SilentlyContinue |
    ForEach-Object { (Get-ItemProperty $_.PSPath).Moniker } |
    Where-Object { $_ -like "hx-*" })
}

$before = Get-HxProfiles

# 开一个会话然后**强杀**引擎：析构函数不会跑，profile 必然留下。
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
Check "强杀之后确实留下了 profile（否则这条测试是空的）" ($leaked.Count -gt 0) "leaked=$($leaked.Count)"

# 再起一个引擎：它的启动清扫应该把上面那条收掉。
Run-Hx -Lines @((Open-Session)) | Out-Null
Start-Sleep -Milliseconds 400
$after = Get-HxProfiles
$still = @($leaked | Where-Object { $after -contains $_ })
Check "下一次启动把残留清掉了" ($still.Count -eq 0) "still=$($still -join ',')"

Write-Host ""
Write-Host "pass=$($script:pass) fail=$($script:fail)"
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $ws "out.tmp"), (Join-Path $ws "made.txt"), (Join-Path $ws "ro.tmp")
exit $script:fail
