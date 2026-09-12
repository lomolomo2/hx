# The Windows try-it entry point for hx -- one command to see what it actually
# does.
#
#   pwsh try-hx.ps1                 # offline: a fake model drives a full multi-step task
#   pwsh try-hx.ps1 -Sandbox        # the sandbox only: what gets through, what the kernel blocks
#   pwsh try-hx.ps1 -Caps           # this machine's isolation capability self-test
#   pwsh try-hx.ps1 -Repl           # feed hxp/0 requests to the engine by hand and see the raw replies
#
#   # Switch to a real model (any OpenAI-compatible endpoint):
#   pwsh try-hx.ps1 -BaseUrl https://192.168.1.241/llm/v1 -Model qwen3.8-27b-uncensored `
#        -ContextWindow 32768 -Root C:\path\to\repo -Prompt "fix the failing tests"
#
#   If the endpoint uses a self-signed certificate, put the PEM at
#   ~\.hx\certs\llm-<hostname>.pem and the script finds it and trusts **only**
#   it (rather than switching off TLS verification globally).
#
# This is a convenience script for trying things out. It takes no part in the
# build or the regression tests, and deleting it affects nothing.
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
  # The PEM for a self-signed certificate. Without it, ~\.hx\certs\llm-<host>.pem
  # is looked up automatically.
  [string]$CaCert = ""
)

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$hxd = Join-Path $repo "engine\build\hxd.exe"

function Need-Engine {
  if (-not (Test-Path $hxd)) {
    Write-Host "hxd.exe is missing; building first..." -ForegroundColor Yellow
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
  Title "this machine's isolation capabilities (--self-test)"
  $json = & $hxd --self-test
  $code = $LASTEXITCODE
  $json | ConvertFrom-Json | ConvertTo-Json -Depth 6
  Write-Host ""
  if ($code -eq 0) { Write-Host "exit code 0 = this machine has a real sandbox" -ForegroundColor Green }
  else { Write-Host "exit code $code = no real sandbox (the engine reports honestly and never pretends to be safe)" -ForegroundColor Red }
  exit $code
}

# ---------------------------------------------------------------- repl
if ($Repl) {
  Need-Engine
  Title "hxp/0 direct (one JSON request per line, Ctrl+C to exit)"
  Write-Host "examples:"
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
  Title "the sandbox in practice: what should pass gets through / what should be refused is refused"
  Write-Host "(via engine\tests\windows\smoke.ps1, 24 checks)" -ForegroundColor DarkGray
  pwsh -NoProfile -File (Join-Path $repo "engine\tests\windows\smoke.ps1") -Engine $hxd
  exit $LASTEXITCODE
}

# ---------------------------------------------------------------- run a task
Need-Engine

$useMock = [string]::IsNullOrWhiteSpace($BaseUrl)
$ws = $Root

if ($useMock) {
  if (-not $ws) {
    # With no workspace given, build one: a function that computes the wrong
    # answer plus a test that fails
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
  if (-not $Prompt) { throw "-Prompt is required with a real model" }
  if (-not $Model) { throw "-Model is required with a real model" }
}

Title "workspace: $ws"
Get-ChildItem $ws -File | ForEach-Object { Write-Host ("  " + $_.Name) }
$target = Join-Path $ws "calc.ps1"
if (Test-Path $target) {
  Write-Host ""
  Write-Host "  calc.ps1 currently looks like this:" -ForegroundColor DarkGray
  Get-Content $target | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }
}

$mock = $null
try {
  if ($useMock) {
    Title "starting an offline OpenAI-compatible endpoint (host\test\mock-model.mjs)"
    Write-Host "  It replaces only the decision of what to do next; prompts, policy, approval and engine calls all take the real path." -ForegroundColor DarkGray
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

    # ★ Self-signed certificates: Node's fetch refuses them outright with
    #   DEPTH_ZERO_SELF_SIGNED_CERT.
    #   The right approach is NODE_EXTRA_CA_CERTS -- trust **only** this one
    #   certificate, rather than NODE_TLS_REJECT_UNAUTHORIZED=0, which switches
    #   off TLS verification for the whole process. The latter also leaves this
    #   process defenceless against any man in the middle, a price far beyond
    #   "connect to this one machine".
    $pem = $CaCert
    if (-not $pem) {
      $host_ = ([Uri]$BaseUrl).Host
      $guess = Join-Path $env:USERPROFILE ".hx\certs\llm-$host_.pem"
      if (Test-Path $guess) { $pem = $guess }
    }
    if ($pem) {
      if (-not (Test-Path $pem)) { throw "CA cert not found: $pem" }
      $env:NODE_EXTRA_CA_CERTS = (Resolve-Path $pem).Path
      Write-Host "  TLS: trusting only $($env:NODE_EXTRA_CA_CERTS)" -ForegroundColor DarkGray
    }
  }
  $env:HX_APPROVAL = $Approval
  # ★ Must match the server's n_ctx: too high and the server truncates
  #   silently, too low and compaction kicks in early.
  #   llama.cpp exposes the real value at /props.
  if ($ContextWindow -gt 0) { $env:HX_CONTEXT_WINDOW = "$ContextWindow" }

  Title "running the task: $Prompt"
  Push-Location (Join-Path $repo "host")
  npx tsx src/cli.ts --root $ws --sandbox workspace-write --net deny --max-steps $MaxSteps $Prompt
  $rc = $LASTEXITCODE
  Pop-Location
} finally {
  if ($mock) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
}

if (Test-Path $target) {
  Title "calc.ps1 after the task"
  Get-Content $target | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor Green }

  Title "independent check: run the test again yourself, outside the sandbox"
  Push-Location $ws
  powershell -NoProfile -ExecutionPolicy Bypass -File test_calc.ps1
  Write-Host "  verify exit=$LASTEXITCODE"
  Pop-Location
}

Title "the session log (the source of truth, append-only)"
$roll = Get-ChildItem (Join-Path $env:USERPROFILE ".hx\sessions") -Recurse -Filter "rollout-*.jsonl" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
if ($roll) {
  Write-Host "  $($roll.FullName)"
  Write-Host "  The first record nails down whether this run had a real sandbox:" -ForegroundColor DarkGray
  $meta = (Get-Content $roll.FullName -TotalCount 1 | ConvertFrom-Json)
  Write-Host ("    sandbox  = " + $meta.payload.effective.sandbox + " / " + $meta.payload.effective.net)
  Write-Host ("    enforced = " + $meta.payload.effective.enforced + "   backend = " + $meta.payload.effective.backend)
}
Write-Host ""
