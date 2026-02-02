$p = Start-Process -FilePath "E:\Projects\github\Live\R3000-Emu\build_dbg\Debug\r3000_emu.exe" `
    -ArgumentList "--bios=E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin","--cd=E:\Projects\github\Live\R3000-Emu\ridgeracer.cue" `
    -WorkingDirectory "E:\Projects\github\Live\R3000-Emu" `
    -RedirectStandardOutput "E:\Projects\github\Live\R3000-Emu\stdout.txt" `
    -RedirectStandardError "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 150
if (-not $p.HasExited) { $p.Kill() }
Write-Host "=== POST-MEMTEST CLEAR ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "Post-memtest" | Select-Object -ExpandProperty Line
Write-Host "`n=== CRASHES ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "CRASH" | Select-Object -First 5 -ExpandProperty Line
Write-Host "`n=== CDROM COMMANDS ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "CMD exec|INT\d" | Select-Object -First 20 -ExpandProperty Line
Write-Host "`n=== BIOS CALLS (filtered) ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "\[BIOS\]" |
    Where-Object { $_.Line -notmatch "B\(0x3D\)" -and $_.Line -notmatch "A\(0x25\)" -and $_.Line -notmatch "A\(0x3F\)" -and $_.Line -notmatch "A\(0x17\)" } |
    Select-Object -First 50 -ExpandProperty Line
