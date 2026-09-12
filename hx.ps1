# hx —— 像 claude 一样用：在任意仓库目录里敲 `hx` 就进交互模式。
#
#   hx                          # 交互（当前目录作为工作区）
#   hx "把 tests 里失败的用例修好"   # 一次性跑完就退
#   hx -Net allow -Approval auto
#
# 端点配置的优先级：命令行 > 环境变量 > ~\.hx\config.json
#
#   ~\.hx\config.json
#   {
#     "baseUrl": "https://192.168.1.241/llm/v1",
#     "model": "qwen3.8-27b-uncensored",
#     "contextWindow": 32768,
#     "apiKey": ""
#   }
#
# 自签证书：PEM 放到 ~\.hx\certs\llm-<主机名>.pem，本脚本会**只**信任它
# （NODE_EXTRA_CA_CERTS），而不是把整个进程的 TLS 校验关掉。
param(
  # ★ Position=0 不能省。只写 ValueFromRemainingArguments 的话，
  #   `hx "任务"` 里那个裸参数会去绑**下一个**可按位置绑定的参数（实测绑到了
  #   -Approval），于是任务凭空消失、直接报 usage。
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
  [string]$CaCert = ""
)

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$hxd = Join-Path $repo "engine\build\hxd.exe"
# ★ 不能用 `npx tsx`：npx 从**当前目录**往上找 node_modules，而 hx 就是要在
#   别人的仓库里跑。直接指向 host 自己的 tsx，才和 cwd 无关。
$tsx = Join-Path $repo "host\node_modules\.bin\tsx.cmd"

if (-not (Test-Path $hxd)) {
  Write-Host "hxd.exe 不在，先构建..." -ForegroundColor Yellow
  pwsh -NoProfile -File (Join-Path $repo "engine\build-win.ps1")
}
if (-not (Test-Path $tsx)) { throw "tsx 不在：先在 $repo\host 里跑一次 npm install" }

# ------------------------------------------------------------------ 配置
$cfgPath = Join-Path $env:USERPROFILE ".hx\config.json"
$cfg = if (Test-Path $cfgPath) { Get-Content $cfgPath -Raw | ConvertFrom-Json } else { $null }

function Pick($cli, $envName, $cfgValue) {
  if ($cli) { return $cli }
  # ★ `$env:$envName` 是语法错误（env: 驱动不接受变量名插值），必须走 .NET。
  $fromEnv = [Environment]::GetEnvironmentVariable($envName)
  if ($fromEnv) { return $fromEnv }
  if ($cfgValue) { return "$cfgValue" }
  return ""
}

$url = Pick $BaseUrl "HX_BASE_URL" $cfg.baseUrl
$mdl = Pick $Model   "HX_MODEL"    $cfg.model
$key = Pick $ApiKey  "HX_API_KEY"  $cfg.apiKey
if (-not $url -or -not $mdl) {
  Write-Host "没有模型端点。给 -BaseUrl/-Model，或设 HX_BASE_URL/HX_MODEL，" -ForegroundColor Red
  Write-Host "或写一个 $cfgPath：" -ForegroundColor Red
  Write-Host '  { "baseUrl": "https://host/v1", "model": "your-model", "contextWindow": 32768 }' -ForegroundColor DarkGray
  exit 78
}

$ctx = if ($ContextWindow -gt 0) { $ContextWindow } elseif ($env:HX_CONTEXT_WINDOW) { [int]$env:HX_CONTEXT_WINDOW } elseif ($cfg.contextWindow) { [int]$cfg.contextWindow } else { 0 }

$env:HX_BASE_URL = $url
$env:HX_MODEL = $mdl
if ($key) { $env:HX_API_KEY = $key }
$env:HX_APPROVAL = $Approval
# ★ 必须与服务端 n_ctx 对齐：设大了会被服务端静默截断（历史悄悄丢），
#   设小了会过早压缩、白扔信息。llama.cpp 可以从 /props 读到真实值。
if ($ctx -gt 0) { $env:HX_CONTEXT_WINDOW = "$ctx" }

# 自签证书
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

# ------------------------------------------------------------------ 跑
if (-not $Root) { $Root = (Get-Location).Path }
$Root = (Resolve-Path $Root).Path

$argv = @(
  (Join-Path $repo "host\src\cli.ts")
  "--root", $Root
  "--sandbox", $Sandbox
  "--net", $Net
  "--max-steps", "$MaxSteps"
)
foreach ($p in $ReadPath) { $argv += @("--read-path", (Resolve-Path $p).Path) }
if ($Task) { $argv += ($Task -join " ") }

& $tsx @argv
exit $LASTEXITCODE
