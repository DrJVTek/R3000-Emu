$ErrorActionPreference = 'SilentlyContinue'
Set-Location "E:\Projects\github\Live\R3000-Emu"

& .\build_dbg\Debug\r3000_emu.exe `
    --bios="bios/ps1_bios.bin" `
    --cd="E:\Projects\PSX\roms\Ridge Racer (U).cue" `
    --hle `
    --max-steps=15000000 `
    --emu-log-level=warn 2>&1 | Tee-Object -Variable output

Write-Host "`n=== SetLoc entries ===" -ForegroundColor Yellow
$output | Select-String -Pattern "SETLOC|GARBAGE"
