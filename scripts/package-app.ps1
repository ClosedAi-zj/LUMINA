# LUMINA beta packaging: assemble Electron runtime + renderer bundle + C++ engine
# into a self-contained folder that can be launched by double-clicking LUMINA.exe.
#
# Layout (matches the appRoot convention in main.ts: main.cjs is 3 levels below resources/):
#   build/pkg/LUMINA/
#     LUMINA.exe                         <- renamed electron.exe
#     *.dll / *.pak / *.dat / locales/   <- Electron runtime
#     resources/
#       app/package.json                 <- main points to ../build/app/electron/main.cjs
#       build/app/                       <- vite renderer bundle + electron main process
#       build/engine/lumina-engine.exe   <- C++ engine (statically linked)
#
# IMPORTANT: keep this file pure ASCII. Windows PowerShell 5.1 decodes BOM-less
# scripts as ANSI; non-ASCII comments corrupt line parsing and silently swallow
# statements (observed: "$defaultApp" left undefined -> Test-Path null binding error).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-app.ps1 [-SkipBuild] [-Zip]

param(
    [switch]$SkipBuild,
    [switch]$Zip
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $root 'app'
$outRoot = Join-Path $root 'build/pkg'
$pkgDir = Join-Path $outRoot 'LUMINA'

Write-Host "[pkg] root = $root"

if (-not $SkipBuild) {
    Write-Host "[pkg] building renderer + electron ..."
    Push-Location $appDir
    try {
        npm run build
        if ($LASTEXITCODE -ne 0) { throw "npm run build failed" }
    } finally {
        Pop-Location
    }
}

# ---- preflight: refuse to build a package that cannot actually run ----
$electronDist = Join-Path $appDir 'node_modules/electron/dist'
$rendererIndex = Join-Path $root 'build/app/index.html'
$mainCjs = Join-Path $root 'build/app/electron/main.cjs'
$preloadCjs = Join-Path $root 'build/app/electron/preload.cjs'
$engineExe = Join-Path $root 'build/engine/lumina-engine.exe'
$readmeName = [string]([char]0x4F7F) + [char]0x7528 + [char]0x8BF4 + [char]0x660E + '.txt'
$readmeSrc = Join-Path $appDir $readmeName

$required = @($electronDist, $rendererIndex, $mainCjs, $preloadCjs, $engineExe, $readmeSrc)
foreach ($p in $required) {
    if ([string]::IsNullOrWhiteSpace($p)) { throw "preflight: empty path in required list" }
    if (-not (Test-Path -LiteralPath $p)) { throw "missing prerequisite: $p" }
}

if (Test-Path -LiteralPath $pkgDir) {
    Write-Host "[pkg] cleaning $pkgDir"
    Remove-Item -LiteralPath $pkgDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null

# ---- 1. Electron runtime ----
Write-Host "[pkg] copying electron runtime ..."
Copy-Item -Path (Join-Path $electronDist '*') -Destination $pkgDir -Recurse -Force
$renamedExe = Join-Path $pkgDir 'LUMINA.exe'
Move-Item -LiteralPath (Join-Path $pkgDir 'electron.exe') -Destination $renamedExe -Force
# default_app.asar is Electron's bundled demo app; leaving it in place makes a broken
# launch look like a working one.
$defaultApp = Join-Path $pkgDir 'resources/default_app.asar'
if (Test-Path -LiteralPath $defaultApp) { Remove-Item -LiteralPath $defaultApp -Force }

# ---- 1b. 体积精简：Electron 自带 100+ 个语言 pak（约 41MB），本产品只需中英两门。
# 其余语言删除后 Electron 自动回退 en-US，不影响功能；保留清单之外的任何 pak 都不随包发布。
$localesDir = Join-Path $pkgDir 'locales'
$keepLocales = @('en-US.pak', 'zh-CN.pak')
if (Test-Path -LiteralPath $localesDir) {
  $removedLocales = 0
  Get-ChildItem -LiteralPath $localesDir -Filter '*.pak' | ForEach-Object {
    if ($keepLocales -notcontains $_.Name) {
      Remove-Item -LiteralPath $_.FullName -Force
      $removedLocales++
    }
  }
  Write-Host "[pkg] locales trimmed: removed $removedLocales pak(s), kept $($keepLocales -join ',')"
  $missing = $keepLocales | Where-Object { -not (Test-Path -LiteralPath (Join-Path $localesDir $_)) }
  if ($missing.Count -gt 0) { throw "[pkg] required locale pak missing after trim: $($missing -join ',')" }
}

# ---- 2. app entry: resources/app/package.json ----
# main must stay INSIDE the app dir. A "../build/..." main silently fails to load
# (Electron resolves main relative to the app root and refuses to escape it), which
# looks like a hung process with no window and no stderr.
$resApp = Join-Path $pkgDir 'resources/app'
New-Item -ItemType Directory -Force -Path $resApp | Out-Null
$pkgJson = @'
{
  "name": "lumina-app",
  "productName": "LUMINA",
  "version": "1.2.3",
  "description": "LUMINA photography workspace",
  "main": "electron/main.cjs"
}
'@
Set-Content -Path (Join-Path $resApp 'package.json') -Value $pkgJson -Encoding ASCII

# ---- 3. renderer bundle (appRoot/build/app) + main process (appRoot = resources/) ----
Write-Host "[pkg] copying renderer + main process ..."
$resBuildApp = Join-Path $pkgDir 'resources/build/app'
New-Item -ItemType Directory -Force -Path $resBuildApp | Out-Null
# build/app/node_modules is a DIRECTORY LINK to app/node_modules (about 420 MB). It is
# needed by the build-time renderer runtime, but it must NOT be shipped: PS 5.1's
# Copy-Item -Recurse follows directory links and copies the target contents, which once
# bloated the portable zip to 716 MB. Enumerate the entries and skip the link itself
# (Where-Object yields the directory entry, not its contents, so nothing is followed).
# Only index.html / assets / electron are real build artifacts here.
Get-ChildItem -LiteralPath (Join-Path $root 'build/app') -Force |
    Where-Object { $_.Name -ne 'node_modules' } |
    Copy-Item -Destination $resBuildApp -Recurse -Force

$resAppElectron = Join-Path $resApp 'electron'
New-Item -ItemType Directory -Force -Path $resAppElectron | Out-Null
Copy-Item -Path (Join-Path $root 'build/app/electron/*') -Destination $resAppElectron -Recurse -Force
# renderer side does not need a second copy of the main process bundle
$dupElectron = Join-Path $resBuildApp 'electron'
if (Test-Path -LiteralPath $dupElectron) { Remove-Item -LiteralPath $dupElectron -Recurse -Force }

# ---- 3b. package hygiene self-check: no node_modules / no link may reach the package ----
# Guard against the 420 MB node_modules link leaking back in (history: 716 MB zip).
$leaked = Join-Path $resBuildApp 'node_modules'
if (Test-Path -LiteralPath $leaked) {
    throw "[pkg] node_modules leaked into package: $leaked"
}
# Any remaining reparse point (link/junction) under the copied renderer is the same bug
# with a different name; fail loudly instead of shipping whatever its target contains.
$leakedLinks = @(Get-ChildItem -LiteralPath $resBuildApp -Recurse -Force -ErrorAction SilentlyContinue |
    Where-Object { ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 })
if ($leakedLinks.Count -gt 0) {
    throw ("[pkg] reparse point leaked into package: {0}" -f $leakedLinks[0].FullName)
}

# ---- 4. C++ engine ----
Write-Host "[pkg] copying engine ..."
$resEngine = Join-Path $pkgDir 'resources/build/engine'
New-Item -ItemType Directory -Force -Path $resEngine | Out-Null
Copy-Item -LiteralPath $engineExe -Destination $resEngine -Force

# ---- 5. end-user notes (source file lives in app/, script stays ASCII-only) ----
Copy-Item -LiteralPath $readmeSrc -Destination (Join-Path $pkgDir $readmeName) -Force

# ---- 6. size report ----
$size = (Get-ChildItem -LiteralPath $pkgDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("[pkg] done -> {0}  ({1:N1} MB)" -f $pkgDir, ($size / 1MB))
Write-Host "[pkg] exe = $renamedExe"

if ($Zip) {
    $zipPath = Join-Path $outRoot 'LUMINA-1.2.3-portable.zip'
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Write-Host "[pkg] compressing -> $zipPath ..."
    Compress-Archive -Path (Join-Path $pkgDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
    $zs = (Get-Item -LiteralPath $zipPath).Length
    Write-Host ("[pkg] zip = {0} ({1:N1} MB)" -f $zipPath, ($zs / 1MB))
}
