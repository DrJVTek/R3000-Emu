$f = [System.IO.File]::OpenRead("E:/Projects/PSX/roms/RIDGE RACER (U).BIN")
$buf = New-Object byte[] 64

# Sector 18 header (first 32 bytes of raw sector)
$null = $f.Seek(18 * 2352, "Begin")
$null = $f.Read($buf, 0, 32)
Write-Host "Sector 18 raw header:"
Write-Host (($buf | Select-Object -First 32 | ForEach-Object { "{0:X2}" -f $_ }) -join " ")

# Sector 22 header + user data
$null = $f.Seek(22 * 2352, "Begin")
$buf2 = New-Object byte[] 96
$null = $f.Read($buf2, 0, 96)
Write-Host "`nSector 22 raw header:"
Write-Host (($buf2 | Select-Object -First 24 | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
Write-Host "Sector 22 user data (first 64 bytes):"
$ud = $buf2[24..87]
Write-Host (($ud | ForEach-Object { "{0:X2}" -f $_ }) -join " ")

$f.Close()
