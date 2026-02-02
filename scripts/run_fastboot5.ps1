$args_str = "--bios=E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin --cd=E:\Projects\github\Live\R3000-Emu\ridgeracer.cue --fast-boot --max-steps=50000000 --emu-log-level=info --pc-sample=10000000"
$p = Start-Process -FilePath "E:\Projects\github\Live\R3000-Emu\build_dbg\Debug\r3000_emu.exe" `
    -ArgumentList $args_str `
    -WorkingDirectory "E:\Projects\github\Live\R3000-Emu" `
    -RedirectStandardOutput "E:\Projects\github\Live\R3000-Emu\stdout.txt" `
    -RedirectStandardError "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 60
if (-not $p.HasExited) { $p.Kill() }
Write-Host "=== SAMPLES ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "SAMPLE" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== SPINS ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "spin=" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== BIOS CALLS ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "\[BIOS\]" |
    Where-Object { $_.Line -notmatch "B\(0x3D\)" -and $_.Line -notmatch "A\(0x25\)" -and $_.Line -notmatch "A\(0x3F\)" -and $_.Line -notmatch "B\(0x17\)" } |
    Select-Object -First 30 -ExpandProperty Line
Write-Host "`n=== GPU ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "\[GPU\]" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== CDROM ==="
Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "CMD 0x" | Select-Object -First 10 -ExpandProperty Line
Write-Host "`n=== LAST ==="
Get-Content "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" | Select-Object -Last 5
