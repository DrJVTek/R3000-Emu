$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root

$exe = Join-Path $root "build_dbg\Debug\r3000_emu.exe"
$bios = "E:\Projects\PSX\psx_bios\SCPH1001.BIN"
$cd = "E:\Projects\PSX\games\Ridge_Racer\Ridge_Racer.cue"
$stderr = Join-Path $root "stderr_push.txt"
$stdout = Join-Path $root "stdout_push.txt"

Write-Host "Running test..."
$p = Start-Process -FilePath $exe `
  -ArgumentList "--bios=$bios","--cd=$cd","--hle","--max-steps=2000000","--emu-log-level=info" `
  -WorkingDirectory $root `
  -RedirectStandardOutput $stdout `
  -RedirectStandardError $stderr `
  -PassThru -NoNewWindow

$p.WaitForExit()
Write-Host "ExitCode: $($p.ExitCode)"

Write-Host ""
Write-Host "=== CDROM IRQ push test ==="
Select-String -Path $stderr -Pattern "CDROM IRQ push|CDROM IRQ edge|CD set_irq|IRQ_ACK" -ErrorAction SilentlyContinue |
  Select-Object -First 30 -ExpandProperty Line

Write-Host ""
Write-Host "=== Last 30 lines ==="
Get-Content $stderr -Tail 30
