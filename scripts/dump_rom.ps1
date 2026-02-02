$b = [System.IO.File]::ReadAllBytes('E:\Projects\github\Live\R3000-Emu\bios\ps1_bios.bin')
$off = 0x69240
for ($i = 0; $i -lt 32; $i += 4) {
    $w = [BitConverter]::ToUInt32($b, $off + $i)
    Write-Host ('{0:X8}: {1:X8}' -f ($off + $i), $w)
}
