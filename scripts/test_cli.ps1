$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root

$exe = Join-Path $root "build_dbg\\Debug\\r3000_emu.exe"
if (-not (Test-Path $exe)) {
  throw "Missing exe: $exe (run scripts/build_dbg.ps1 first)"
}

$bios = Join-Path $root "bios\\ps1_bios.bin"
if (-not (Test-Path $bios)) {
  throw "Missing BIOS: $bios"
}

$cd = Join-Path $root "ridgeracer.cue"
if (-not (Test-Path $cd)) {
  throw "Missing CD cue: $cd"
}

function Run-Test([string]$name, [string[]]$argList, [string]$stderrPath) {
  Write-Host ""
  Write-Host "=== $name ==="
  $argList = @($argList | Where-Object { $_ -ne $null -and $_.Length -gt 0 })
  Write-Host "Args: $($argList -join ' ')"

  if (Test-Path $stderrPath) { Remove-Item $stderrPath -Force }

  $p = Start-Process -FilePath $exe `
    -ArgumentList ($argList -join ' ') `
    -WorkingDirectory $root `
    -RedirectStandardOutput (Join-Path $root "stdout_test.txt") `
    -RedirectStandardError $stderrPath `
    -PassThru -NoNewWindow

  $p.WaitForExit()
  $ec = [int]$p.ExitCode
  Write-Host "ExitCode: $ec"
  return $ec
}

# 1) Fast boot from CD (matches your CLI scripts)
$err1 = Join-Path $root "stderr_fastboot_test.txt"
$rc1 = Run-Test "FASTBOOT" @(
  "--bios=$bios",
  "--cd=$cd",
  "--fast-boot",
  "--max-steps=8000000",
  "--emu-log-level=info"
) $err1

# 2) BIOS boot with CD inserted (no fast-boot). This is the UE5 problem path.
$err2 = Join-Path $root "stderr_bioscd_test.txt"
$rc2 = Run-Test "BIOS+CD (no fastboot)" @(
  "--bios=$bios",
  "--cd=$cd",
  "--max-steps=12000000",
  "--emu-log-level=info"
) $err2

# 3) BIOS+CD with HLE vectors (matches UE5 defaults).
$err3 = Join-Path $root "stderr_bioscd_hle_test.txt"
$rc3 = Run-Test "BIOS+CD (HLE vectors ON)" @(
  "--bios=$bios",
  "--cd=$cd",
  "--hle",
  "--max-steps=20000000",
  "--pc-sample=5000000",
  "--emu-log-level=info"
) $err3

Write-Host ""
Write-Host "=== Grep highlights (FASTBOOT) ==="
Select-String -Path $err1 -Pattern "Fast boot|CORE|EXE found|SYSTEM\\.CNF|ISO9660|Auto-enable I_MASK|HALT|Illegal|fault" -SimpleMatch -ErrorAction SilentlyContinue |
  Select-Object -First 60 -ExpandProperty Line

Write-Host ""
Write-Host "=== Grep highlights (BIOS+CD) ==="
Select-String -Path $err2 -Pattern "insert_disc|ISO9660|Auto-enable I_MASK|HLE WaitEvent|VSync|timeout|HALT|Illegal|fault" -ErrorAction SilentlyContinue |
  Select-Object -First 60 -ExpandProperty Line

Write-Host ""
Write-Host "=== Grep highlights (BIOS+CD HLE) ==="
Select-String -Path $err3 -Pattern "SAMPLE step=|insert_disc|ISO9660|Auto-enable I_MASK|HLE WaitEvent|VSync|timeout|HALT|Illegal|fault" -ErrorAction SilentlyContinue |
  Select-Object -First 80 -ExpandProperty Line

Write-Host ""
Write-Host "Done."

