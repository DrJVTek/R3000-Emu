$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root

Write-Host "=== Configure (build_dbg) ==="
cmake -S . -B build_dbg

Write-Host "=== Build Debug ==="
cmake --build build_dbg --config Debug

Write-Host "=== Done ==="

