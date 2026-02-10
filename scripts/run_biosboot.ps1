# Run Ridge Racer with FULL BIOS boot (no fast-boot)
$args_str = "--bios=E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin --cd=E:\Projects\github\Live\R3000-Emu\ridgeracer.cue --hle --max-steps=200000000 --max-time=120 --emu-log-level=info --pc-sample=50000000"
$p = Start-Process -FilePath "E:\Projects\github\Live\R3000-Emu\build_dbg\Debug\r3000_emu.exe" `
    -ArgumentList $args_str `
    -WorkingDirectory "E:\Projects\github\Live\R3000-Emu" `
    -RedirectStandardOutput "E:\Projects\github\Live\R3000-Emu\stdout_biosboot.txt" `
    -RedirectStandardError "E:\Projects\github\Live\R3000-Emu\stderr_biosboot.txt" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 120
if (-not $p.HasExited) { $p.Kill() }
Write-Host "=== SAMPLES ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_biosboot.txt" -Pattern "SAMPLE" | Select-Object -First 5 -ExpandProperty Line
Write-Host "`n=== File not found ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_biosboot.txt" -Pattern "File not found" | Select-Object -First 3 -ExpandProperty Line
Write-Host "`n=== SetLoc ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_biosboot.txt" -Pattern "DIAG-SETLOC" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== LAST 10 ==="
Get-Content "E:\Projects\github\Live\R3000-Emu\stderr_biosboot.txt" | Select-Object -Last 10
