$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$src = Join-Path $repo 'Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin'
$dstDir = Join-Path $repo 'bios'
$dst = Join-Path $dstDir 'ps1_bios.bin'

if (-not (Test-Path -LiteralPath $src)) {
  throw "BIOS source file not found: $src"
}

New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
Copy-Item -LiteralPath $src -Destination $dst -Force

Write-Host "OK: copied BIOS to $dst"
