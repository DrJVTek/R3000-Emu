$f = [System.IO.File]::OpenRead("E:/Projects/PSX/roms/RIDGE RACER (U).BIN")
$buf = New-Object byte[] 64
$offset = 18 * 2352 + 24
$null = $f.Seek($offset, "Begin")
$null = $f.Read($buf, 0, 64)
$f.Close()
Write-Host "Sector 18 user data (first 64 bytes):"
Write-Host (($buf | ForEach-Object { "{0:X2}" -f $_ }) -join " ")

# Also dump PVD root dir record (sector 16, offset 156 from user data)
$f2 = [System.IO.File]::OpenRead("E:/Projects/PSX/roms/RIDGE RACER (U).BIN")
$buf2 = New-Object byte[] 64
$pvd_offset = 16 * 2352 + 24 + 156
$null = $f2.Seek($pvd_offset, "Begin")
$null = $f2.Read($buf2, 0, 64)
$f2.Close()
Write-Host "`nPVD root dir record (at PVD+156):"
Write-Host (($buf2 | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
# Decode root dir LBA (bytes 2-5 LE)
$rootLBA = [BitConverter]::ToUInt32($buf2, 2)
$rootSize = [BitConverter]::ToUInt32($buf2, 10)
Write-Host "Root dir LBA: $rootLBA  Size: $rootSize"
