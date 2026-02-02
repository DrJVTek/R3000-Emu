$args_str = "--bios=E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin --cd=E:\Projects\github\Live\R3000-Emu\ridgeracer.cue --hle --max-steps=50000000 --emu-log-level=info"
$p = Start-Process -FilePath "E:\Projects\github\Live\R3000-Emu\build_dbg\Debug\r3000_emu.exe" `
    -ArgumentList $args_str `
    -WorkingDirectory "E:\Projects\github\Live\R3000-Emu" `
    -RedirectStandardOutput "E:\Projects\github\Live\R3000-Emu\stdout.txt" `
    -RedirectStandardError "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 60
if (-not $p.HasExited) { $p.Kill() }
Write-Host "=== BIOS CALLS ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "\[BIOS\]" |
    Where-Object { $_.Line -notmatch "B\(0x3D\)" -and $_.Line -notmatch "A\(0x25\)" -and $_.Line -notmatch "A\(0x3F\)" -and $_.Line -notmatch "A\(0x17\)" } |
    Select-Object -First 40 -ExpandProperty Line
Write-Host "`n=== CDROM ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "CMD exec|CD read|CdInit|CdRead" | Select-Object -First 20 -ExpandProperty Line
Write-Host "`n=== CRASHES ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "CRASH|Illegal" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== GPU ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "GPU|GP0|GP1" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== LAST LINES ==="
Get-Content "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" | Select-Object -Last 20
