$ErrorActionPreference = 'SilentlyContinue'
Set-Location "E:\Projects\github\Live\R3000-Emu"

$cdPath = 'E:\Projects\PSX\roms\Ridge Racer (U).cue'
$args = @(
    '--bios=bios/ps1_bios.bin',
    "--cd=`"$cdPath`"",
    '--max-steps=50000000',
    '--emu-log-level=info'
)

# Run without HLE to simulate UE5 behavior (real BIOS)
& .\build_dbg\Debug\r3000_emu.exe $args 2>workbench\stderr_realbios.txt | Out-Null

Write-Host "`n=== Checking for CDROM errors ===" -ForegroundColor Yellow
Get-Content "workbench\stderr_realbios.txt" | Select-String -Pattern "stop_reading|beyond disc|GARBAGE|ERROR_REASON" | Select-Object -First 20

Write-Host "`n=== Last 30 lines ===" -ForegroundColor Yellow
Get-Content "workbench\stderr_realbios.txt" | Select-Object -Last 30
