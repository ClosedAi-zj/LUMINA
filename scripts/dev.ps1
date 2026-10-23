# LUMINA 开发模式：并行启动 vite dev server + Electron。
# 用法：pwsh scripts/dev.ps1

$ErrorActionPreference = 'Stop'

# 工具链路径（Node v22 本地版本）
$env:PATH = "C:/Users/zj/.workbuddy/binaries/node/versions/22.22.2-6;$env:PATH"

# Electron 二进制国内镜像（首次 npm install 时使用）
$env:ELECTRON_MIRROR = "https://npmmirror.com/mirrors/electron/"

# 开发标识：主进程加载 http://localhost:5173
$env:LUMINA_DEV = "1"
$env:LUMINA_HOME = "D:/LUMINA/build/runtime"

# 引擎 exe 路径（存在则 spawn 真引擎，否则自动进入 Mock 模式）
# $env:LUMINA_ENGINE = "D:/LUMINA/build/engine/lumina-engine.exe"

Write-Host "[dev] 安装依赖（如缺失）…" -ForegroundColor Cyan
if (-not (Test-Path "D:/LUMINA/app/node_modules")) {
  Push-Location "D:/LUMINA/app"
  npm install
  Pop-Location
}

Write-Host "[dev] 编译 Electron 主进程…" -ForegroundColor Cyan
Push-Location "D:/LUMINA/app"
npm run build:electron
Pop-Location

Write-Host "[dev] 启动 vite dev server…" -ForegroundColor Cyan
$vite = Start-Process -FilePath "npm" -ArgumentList "run","dev:vite" -WorkingDirectory "D:/LUMINA/app" -PassThru -WindowStyle Hidden

# 等待 vite 就绪
$ok = $false
for ($i = 0; $i -lt 30; $i++) {
  try {
    $resp = Invoke-WebRequest -Uri "http://localhost:5173" -UseBasicParsing -TimeoutSec 1
    if ($resp.StatusCode -eq 200) { $ok = $true; break }
  } catch { Start-Sleep -Milliseconds 500 }
}
if (-not $ok) {
  Write-Host "[dev] vite 未能在 15s 内就绪，退出" -ForegroundColor Red
  Stop-Process -Id $vite.Id -Force -ErrorAction SilentlyContinue
  exit 1
}

Write-Host "[dev] 启动 Electron…" -ForegroundColor Cyan
Push-Location "D:/LUMINA/app"
try {
  npm run start
} finally {
  Pop-Location
  Stop-Process -Id $vite.Id -Force -ErrorAction SilentlyContinue
}
