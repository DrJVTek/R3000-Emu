$p = Start-Process -FilePath "E:\Projects\github\Live\R3000-Emu\build_dbg\Debug\r3000_emu.exe" `
    -ArgumentList "--bios=E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin","--cd=E:\Projects\github\Live\R3000-Emu\ridgeracer.cue" `
    -WorkingDirectory "E:\Projects\github\Live\R3000-Emu" `
    -RedirectStandardOutput "E:\Projects\github\Live\R3000-Emu\stdout.txt" `
    -RedirectStandardError "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 130
if (-not $p.HasExited) { $p.Kill() }
Write-Host "=== FULL TRACE ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "\[DIAG\].*\[-" | Select-Object -ExpandProperty Line
Write-Host "`n=== WATCHPOINTS ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "\[WP\]" | Select-Object -First 40 -ExpandProperty Line
Write-Host "`n=== REGS ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "Regs:" | Select-Object -First 5 -ExpandProperty Line
