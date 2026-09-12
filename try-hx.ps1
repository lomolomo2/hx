# hx 的 Windows 试跑入口 —— 一条命令看到它到底在干什么。
#
#   pwsh try-hx.ps1                 # 离线：假模型驱动一个完整的多步任务
#   pwsh try-hx.ps1 -Sandbox        # 只看沙箱：哪些放行、哪些被内核挡掉
#   pwsh try-hx.ps1 -Caps           # 本机隔离能力自检
#   pwsh try-hx.ps1 -Repl           # 手工往引擎里灌 hxp/0 请求，看原始回复
#
#   # 换成真模型（任何 OpenAI 兼容端点）：
#   pwsh try-hx.ps1 -BaseUrl https://192.168.1.241/llm/v1 -Model qwen3.8-27b-uncensored `
#        -ContextWindow 32768 -Root C:\path\to\repo -Prompt "把 tests 里失败的用例修好"
#
#   端点是自签证书的话，把 PEM 放到 ~\.hx\certs\llm-<主机名>.pem，
#   脚本会自己找到并**只**信任它（不是关掉全局 TLS 校验）。
#
# 这是个方便试用的脚本，不参与构建也不参与回归测试，删掉不影响任何东西。
param(
  [switch]$Sandbox,
  [switch]$Caps,
  [switch]$Repl,
  [string]$BaseUrl = "",
  [string]$Model = "",
  [string]$ApiKey = "",
  [string]$Prompt = "",
  [string]$Root = "",
  [string]$Approval = "auto",
  [int]$MaxSteps = 20,
  [int]$ContextWindow = 0,
  # 自签证书的 PEM。不给的话会自己去 ~\.hx\certs\llm-<host>.pem 找。
  [string]$CaCert = ""
)

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$hxd = Join-Path $repo "engine\build\hxd.exe"

function Need-Engine {
  if (-not (Test-Path $hxd)) {
    Write-Host "hxd.exe 不在，先构建..." -ForegroundColor Yellow
    pwsh -NoProfile -File (Join-Path $repo "engine\build-win.ps1")
  }
}

function Title($t) {
  Write-Host ""
  Write-Host ("─" * 66) -ForegroundColor DarkGray
  Write-Host "  $t" -ForegroundColor Cyan
  Write-Host ("─" * 66) -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- caps
if ($Caps) {
  Need-Engine
  Title "本机隔离能力（--self-test）"
  $json = & $hxd --self-test
  $code = $LASTEXITCODE
  $json | ConvertFrom-Json | ConvertTo-Json -Depth 6
  Write-Host ""
  if ($code -eq 0) { Write-Host "退出码 0 = 这台机器上有真沙箱" -ForegroundColor Green }
  else { Write-Host "退出码 $code = 没有真沙箱（引擎会如实上报，不会假装安全）" -ForegroundColor Red }
  exit $code
}

# ---------------------------------------------------------------- repl
if ($Repl) {
  Need-Engine
  Title "hxp/0 直连（每行一个 JSON 请求，Ctrl+C 退出）"
  Write-Host "示例："
  Write-Host '  {"id":"1","op":"ping"}' -ForegroundColor DarkGray
  Write-Host '  {"id":"2","op":"session.open","args":{"roots":["C:\\some\\dir"],"sandbox":"workspace-write","net":"deny"}}' -ForegroundColor DarkGray
  Write-Host '  {"id":"3","op":"exec.start","args":{"cmd":["cmd","/c","echo hi"],"timeout_ms":5000}}' -ForegroundColor DarkGray
  Write-Host '  {"id":"4","op":"exec.wait","args":{"cell":"c1","yield_ms":2000}}' -ForegroundColor DarkGray
  Write-Host ""
  & $hxd
  exit $LASTEXITCODE
}

# ---------------------------------------------------------------- sandbox
if ($Sandbox) {
  Need-Engine
  Title "沙箱实况：该放的放得通 / 该拒的拒掉"
  Write-Host "（用 engine\tests\windows\smoke.ps1，24 项）" -ForegroundColor DarkGray
  pwsh -NoProfile -File (Join-Path $repo "engine\tests\windows\smoke.ps1") -Engine $hxd
  exit $LASTEXITCODE
}

# ---------------------------------------------------------------- 跑一个任务
Need-Engine

$useMock = [string]::IsNullOrWhiteSpace($BaseUrl)
$ws = $Root

if ($useMock) {
  if (-not $ws) {
    # 没指定工作区就造一个：一个算错的函数 + 一个会失败的测试
    $ws = Join-Path $env:TEMP "hx-playground"
    Remove-Item -Recurse -Force $ws -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $ws | Out-Null
    Set-Content -Path (Join-Path $ws "calc.ps1") -Encoding utf8 -Value @(
      'function Add-Values($a, $b) {'
      '    return $a - $b'
      '}'
    )
    Set-Content -Path (Join-Path $ws "test_calc.ps1") -Encoding utf8 -Value @(
      "`$ErrorActionPreference = 'Stop'"
      '. "$PSScriptRoot\calc.ps1"'
      '$r = Add-Values 2 3'
      'if ($r -ne 5) { Write-Output "FAIL: got $r"; exit 1 }'
      "Write-Output 'ALL TESTS PASS'"
    )
  }
  if (-not $Prompt) { $Prompt = "Add-Values is broken. Run the test, find the bug, fix it, and verify." }
} else {
  if (-not $ws) { $ws = (Get-Location).Path }
  if (-not $Prompt) { throw "用真模型时必须给 -Prompt" }
  if (-not $Model) { throw "用真模型时必须给 -Model" }
}

Title "工作区：$ws"
Get-ChildItem $ws -File | ForEach-Object { Write-Host ("  " + $_.Name) }
$target = Join-Path $ws "calc.ps1"
if (Test-Path $target) {
  Write-Host ""
  Write-Host "  calc.ps1 现在长这样：" -ForegroundColor DarkGray
  Get-Content $target | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }
}

$mock = $null
try {
  if ($useMock) {
    Title "起一个离线的 OpenAI 兼容端点（host\test\mock-model.mjs）"
    Write-Host "  它只替换'下一步做什么'这个决策；提示词、策略、审批、引擎调用全走真实路径。" -ForegroundColor DarkGray
    $mock = Start-Process node -ArgumentList (Join-Path $repo "host\test\mock-model.mjs") `
            -PassThru -NoNewWindow `
            -RedirectStandardError (Join-Path $env:TEMP "hx-mock.err") `
            -RedirectStandardOutput (Join-Path $env:TEMP "hx-mock.out")
    Start-Sleep -Milliseconds 900
    $env:HX_BASE_URL = "http://127.0.0.1:4399/v1"
    $env:HX_MODEL = "mock"
  } else {
    $env:HX_BASE_URL = $BaseUrl
    $env:HX_MODEL = $Model
    if ($ApiKey) { $env:HX_API_KEY = $ApiKey }

    # ★ 自签证书：Node 的 fetch 会以 DEPTH_ZERO_SELF_SIGNED_CERT 直接拒掉。
    #   正确做法是 NODE_EXTRA_CA_CERTS —— **只**信任这一张证书，
    #   而不是 NODE_TLS_REJECT_UNAUTHORIZED=0 把整个进程的 TLS 校验关掉。
    #   后者会顺带让这个进程对任何中间人都不设防，代价远超"连上这一台机器"。
    $pem = $CaCert
    if (-not $pem) {
      $host_ = ([Uri]$BaseUrl).Host
      $guess = Join-Path $env:USERPROFILE ".hx\certs\llm-$host_.pem"
      if (Test-Path $guess) { $pem = $guess }
    }
    if ($pem) {
      if (-not (Test-Path $pem)) { throw "CA cert not found: $pem" }
      $env:NODE_EXTRA_CA_CERTS = (Resolve-Path $pem).Path
      Write-Host "  TLS: 只信任 $($env:NODE_EXTRA_CA_CERTS)" -ForegroundColor DarkGray
    }
  }
  $env:HX_APPROVAL = $Approval
  # ★ 必须与服务端 n_ctx 对齐：设大了会被服务端静默截断，设小了会过早压缩。
  #   llama.cpp 可以从 /props 读到真实值。
  if ($ContextWindow -gt 0) { $env:HX_CONTEXT_WINDOW = "$ContextWindow" }

  Title "跑任务：$Prompt"
  Push-Location (Join-Path $repo "host")
  npx tsx src/cli.ts --root $ws --sandbox workspace-write --net deny --max-steps $MaxSteps $Prompt
  $rc = $LASTEXITCODE
  Pop-Location
} finally {
  if ($mock) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
}

if (Test-Path $target) {
  Title "任务之后的 calc.ps1"
  Get-Content $target | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor Green }

  Title "独立复核：在沙箱外自己再跑一次测试"
  Push-Location $ws
  powershell -NoProfile -ExecutionPolicy Bypass -File test_calc.ps1
  Write-Host "  verify exit=$LASTEXITCODE"
  Pop-Location
}

Title "会话日志（事实来源，append-only）"
$roll = Get-ChildItem (Join-Path $env:USERPROFILE ".hx\sessions") -Recurse -Filter "rollout-*.jsonl" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
if ($roll) {
  Write-Host "  $($roll.FullName)"
  Write-Host "  第一条记录钉死了'这次到底有没有真沙箱'：" -ForegroundColor DarkGray
  $meta = (Get-Content $roll.FullName -TotalCount 1 | ConvertFrom-Json)
  Write-Host ("    sandbox  = " + $meta.payload.effective.sandbox + " / " + $meta.payload.effective.net)
  Write-Host ("    enforced = " + $meta.payload.effective.enforced + "   backend = " + $meta.payload.effective.backend)
}
Write-Host ""
