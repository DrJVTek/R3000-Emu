Select-String -Path "E:\Projects\github\Live\R3000-Emu\stderr_debug.txt" -Pattern "\[BIOS\]" |
    Where-Object { $_.Line -notmatch "B\(0x3D\)" -and $_.Line -notmatch "A\(0x25\)" -and $_.Line -notmatch "A\(0x3F\)" -and $_.Line -notmatch "A\(0x17\)" } |
    Select-Object -ExpandProperty Line
