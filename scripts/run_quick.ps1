$args_str = "--bios=E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin --cd=E:\Projects\github\Live\R3000-Emu\ridgeracer.cue --fast-boot --max-steps=5000000 --emu-log-level=debug"
$p = Start-Process -FilePath "E:\Projects\github\Live\R3000-Emu\build_dbg\Debug\r3000_emu.exe" `
    -ArgumentList $args_str `
    -WorkingDirectory "E:\Projects\github\Live\R3000-Emu" `
    -RedirectStandardOutput "E:\Projects\github\Live\R3000-Emu\stdout.txt" `
    -RedirectStandardError "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 30
if (-not $p.HasExited) { $p.Kill() }
Write-Host "=== SECTOR READ ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "read_sector_raw|FIFO FILL" | Select-Object -First 20 -ExpandProperty Line
