# LUMINA engine one-click build (CMake + Ninja + MinGW-w64)
# Usage: pwsh scripts/build-engine.ps1 [-Clean] [-RuntimeHome D:/LUMINA/build/runtime]
param(
    [switch]$Clean,
    [string]$RuntimeHome = "D:/LUMINA/build/runtime"
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/env.ps1"

$root   = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $root 'engine'
$outDir = Join-Path $root 'build/engine'

if ($Clean -and (Test-Path $outDir)) {
    Write-Host "[build] cleaning $outDir"
    Remove-Item -Recurse -Force $outDir
}

Write-Host "[build] cmake configure ..."
cmake -S $srcDir -B $outDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_C_COMPILER=D:/work/tools/mingw_posix/mingw64/bin/gcc.exe" `
    "-DCMAKE_CXX_COMPILER=D:/work/tools/mingw_posix/mingw64/bin/g++.exe"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "[build] cmake build ..."
cmake --build $outDir
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$exe = Join-Path $outDir 'lumina-engine.exe'
if (-not (Test-Path $exe)) { throw "missing output: $exe" }

Write-Host "[build] OK -> $exe"
Write-Host "[build] runtime home = $RuntimeHome"
