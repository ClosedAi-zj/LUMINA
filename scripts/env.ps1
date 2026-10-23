# LUMINA toolchain PATH setup (shared by build/dev scripts)
$ErrorActionPreference = 'Stop'

$env:PATH = "D:/work/tools/mingw_posix/mingw64/bin;C:/Users/zj/.workbuddy/binaries/python/versions/3.13.12/Scripts;" + $env:PATH

function Assert-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "missing tool: $name (check paths in env.ps1)" }
    return $cmd.Source
}

Assert-Tool g++      | Out-Null
Assert-Tool cmake    | Out-Null
Assert-Tool ninja    | Out-Null

Write-Host "[env] g++   = $((Get-Command g++).Source)"
Write-Host "[env] cmake = $((Get-Command cmake).Source)"
Write-Host "[env] ninja = $((Get-Command ninja).Source)"
