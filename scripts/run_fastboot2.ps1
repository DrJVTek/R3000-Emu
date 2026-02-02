$args_str = "--bios=E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin --cd=E:\Projects\github\Live\R3000-Emu\ridgeracer.cue --fast-boot --max-steps=10000000 --emu-log-level=info"
$p = Start-Process -FilePath "E:\Projects\github\Live\R3000-Emu\build_dbg\Debug\r3000_emu.exe" `
    -ArgumentList $args_str `
    -WorkingDirectory "E:\Projects\github\Live\R3000-Emu" `
    -RedirectStandardOutput "E:\Projects\github\Live\R3000-Emu\stdout.txt" `
    -RedirectStandardError "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 30
if (-not $p.HasExited) { $p.Kill() }
Write-Host "=== FAST BOOT ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "Fast boot|EXE found|Loaded text|Run start" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== CRASHES ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "CRASH|fault|Illegal|HALT|failed" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== LAST LINES ==="
Get-Content "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" | Select-Object -Last 15
