# hx -- use it the way you use claude: type `hx` in any repo directory and you
# are in interactive mode.
#
#   hx                              # interactive (the current directory is the workspace)
#   hx "fix the failing tests"      # one-shot: run to completion and exit
#   hx -Net allow -Approval auto
#
# Endpoint configuration precedence: command line > environment > ~\.hx\config.json
#
#   ~\.hx\config.json
#   {
#     "baseUrl": "https://192.168.1.241/llm/v1",
#     "model": "qwen3.8-27b-uncensored",
#     "contextWindow": 32768,
#     "apiKey": ""
#   }
#
# Self-signed certificates: put the PEM at ~\.hx\certs\llm-<hostname>.pem and
# this script trusts **only** it (NODE_EXTRA_CA_CERTS), rather than switching
# off TLS verification for the whole process.
param(
  # ★ Position=0 cannot be omitted. With only ValueFromRemainingArguments, the
  #   bare argument in `hx "task"` binds to the **next** positionally bindable
  #   parameter (measured: it bound to -Approval), so the task vanishes and
  #   usage is printed instead.
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Task,
  [string]$Root = "",
  [string]$BaseUrl = "",
  [string]$Model = "",
  [string]$ApiKey = "",
  [string]$Sandbox = "workspace-write",
  [ValidateSet("deny", "allow")][string]$Net = "deny",
  [string]$Approval = "cautious",
  [int]$MaxSteps = 20,
  [int]$ContextWindow = 0,
  [string[]]$ReadPath = @(),
  [string]$CaCert = "",
  [switch]$Caps
)

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot

# Two layouts: an unpacked release (hxd.exe and the prebuilt bundle sit right
# here), and the repository working tree (the engine has to be built and the
# host runs TypeScript sources through tsx).
$packagedEngine = Join-Path $repo "hxd.exe"
$packagedHost = Join-Path $repo "host\hx-host.mjs"
$packaged = (Test-Path $packagedEngine) -and (Test-Path $packagedHost)

if ($packaged) {
  $hxd = $packagedEngine
  if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw "hx needs Node.js 20+ (the engine is native, the host is JS). https://nodejs.org"
  }
  $runner = "node"
  $entry = $packagedHost
} else {
  $hxd = Join-Path $repo "engine\build\hxd.exe"
  if (-not (Test-Path $hxd)) {
    Write-Host "hxd.exe is missing; building first..." -ForegroundColor Yellow
    pwsh -NoProfile -File (Join-Path $repo "engine\build-win.ps1")
  }
  # ★ `npx tsx` cannot be used: npx searches upward from the **current
  #   directory** for node_modules, and hx exists precisely to run inside
  #   someone else's repo. Pointing straight at host's own tsx is what makes
  #   this independent of cwd.
  $runner = Join-Path $repo "host\node_modules\.bin\tsx.cmd"
  if (-not (Test-Path $runner)) { throw "tsx is missing: run npm install once in $repo\host" }
  $entry = Join-Path $repo "host\src\cli.ts"
}

# ★ The host must be told explicitly where the engine is. By default it looks
#   upward from its own location for ../../engine/build/hxd.exe -- that is the
#   repository layout and simply does not hold inside a release package.
$env:HX_ENGINE = $hxd

# ---------------------------------------------------------------- self-test
# Placed **before** the configuration is read: checking whether the install
# works should not require having configured a model endpoint first.
if ($Caps) {
  $json = & $hxd --self-test
  $code = $LASTEXITCODE
  $json | ConvertFrom-Json | ConvertTo-Json -Depth 6
  Write-Host ""
  if ($code -eq 0) { Write-Host "exit code 0 = this machine has a real sandbox" -ForegroundColor Green }
  else { Write-Host "exit code $code = no real sandbox (the engine reports honestly and never pretends to be safe)" -ForegroundColor Red }
  exit $code
}

# ★ An unrecognised -Flag must be an error on the spot, never handed to the
#   model as a task.
#
#   $Task carries ValueFromRemainingArguments and catches anything at all --
#   including mistyped switches. Measured: `hx -Caps` (before that switch
#   existed) was taken in its entirety as the **task prompt**, so the model
#   earnestly set about researching "how to execute -Caps", spent its whole
#   step budget, burned 338k tokens, and produced a confident but entirely
#   wrong root-cause analysis.
#   A typo in a switch should not become a real model run.
if ($Task -and $Task[0].StartsWith("-")) {
  Write-Host "unrecognised switch: $($Task[0])" -ForegroundColor Red
  Write-Host ""
  Write-Host "available switches: -Caps -Root -Net -Sandbox -Approval -MaxSteps -ReadPath"
  Write-Host "                    -BaseUrl -Model -ApiKey -ContextWindow -CaCert"
  Write-Host ""
  Write-Host "the task itself needs quotes: hx `"fix the failing tests`""
  exit 64
}

# ------------------------------------------------------------------ config
$cfgPath = Join-Path $env:USERPROFILE ".hx\config.json"
$cfg = if (Test-Path $cfgPath) { Get-Content $cfgPath -Raw | ConvertFrom-Json } else { $null }

function Pick($cli, $envName, $cfgValue) {
  if ($cli) { return $cli }
  # ★ `$env:$envName` is a syntax error (the env: drive does not interpolate a
  #   variable name), so this has to go through .NET.
  $fromEnv = [Environment]::GetEnvironmentVariable($envName)
  if ($fromEnv) { return $fromEnv }
  if ($cfgValue) { return "$cfgValue" }
  return ""
}

$url = Pick $BaseUrl "HX_BASE_URL" $cfg.baseUrl
$mdl = Pick $Model   "HX_MODEL"    $cfg.model
$key = Pick $ApiKey  "HX_API_KEY"  $cfg.apiKey
if (-not $url -or -not $mdl) {
  Write-Host "No model endpoint. Pass -BaseUrl/-Model, or set HX_BASE_URL/HX_MODEL," -ForegroundColor Red
  Write-Host "or write a $cfgPath containing:" -ForegroundColor Red
  Write-Host '  { "baseUrl": "https://host/v1", "model": "your-model", "contextWindow": 32768 }' -ForegroundColor DarkGray
  exit 78
}

$ctx = if ($ContextWindow -gt 0) { $ContextWindow } elseif ($env:HX_CONTEXT_WINDOW) { [int]$env:HX_CONTEXT_WINDOW } elseif ($cfg.contextWindow) { [int]$cfg.contextWindow } else { 0 }

$env:HX_BASE_URL = $url
$env:HX_MODEL = $mdl
if ($key) { $env:HX_API_KEY = $key }
$env:HX_APPROVAL = $Approval
# ★ Must match the server's n_ctx: set it too high and the server truncates
#   silently (history quietly disappears); too low and compaction kicks in
#   early, throwing information away for nothing. llama.cpp exposes the real
#   value at /props.
if ($ctx -gt 0) { $env:HX_CONTEXT_WINDOW = "$ctx" }

# Self-signed certificate
$pem = $CaCert
if (-not $pem) {
  $h = ([Uri]$url).Host
  $guess = Join-Path $env:USERPROFILE ".hx\certs\llm-$h.pem"
  if (Test-Path $guess) { $pem = $guess }
}
if ($pem) {
  if (-not (Test-Path $pem)) { throw "CA cert not found: $pem" }
  $env:NODE_EXTRA_CA_CERTS = (Resolve-Path $pem).Path
}

# ------------------------------------------------------------------ run
if (-not $Root) { $Root = (Get-Location).Path }
$Root = (Resolve-Path $Root).Path

$argv = @(
  $entry
  "--root", $Root
  "--sandbox", $Sandbox
  "--net", $Net
  "--max-steps", "$MaxSteps"
)
foreach ($p in $ReadPath) { $argv += @("--read-path", (Resolve-Path $p).Path) }
if ($Task) { $argv += ($Task -join " ") }

& $runner @argv
exit $LASTEXITCODE
