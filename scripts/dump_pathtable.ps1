# Dump path table from ridgeracer CD image
$binPath = "E:\Projects\PSX\roms\RIDGE RACER (U).BIN"
Write-Host "BIN file: $binPath"

$f = [System.IO.File]::OpenRead($binPath)

# Path Table at LBA 18
$buf = New-Object byte[] 64
$offset = 18 * 2352 + 24  # Skip sync + header
$null = $f.Seek($offset, "Begin")
$null = $f.Read($buf, 0, 64)
Write-Host "Path Table (LBA 18) first 64 bytes:"
Write-Host (($buf | ForEach-Object { "{0:X2}" -f $_ }) -join " ")

# First entry: root dir
$nameLen = $buf[0]
$extAttr = $buf[1]
$lba = [BitConverter]::ToUInt32($buf, 2)
$parent = [BitConverter]::ToUInt16($buf, 6)
Write-Host ""
Write-Host "Path Table Entry 1 (root):"
Write-Host "  Name length: $nameLen"
Write-Host "  Extent LBA:  $lba"
Write-Host "  Parent:      $parent"

# PVD root dir record (sector 16, offset 156)
$buf2 = New-Object byte[] 64
$pvd_offset = 16 * 2352 + 24 + 156
$null = $f.Seek($pvd_offset, "Begin")
$null = $f.Read($buf2, 0, 64)
Write-Host ""
Write-Host "PVD root dir record (at PVD+156):"
Write-Host (($buf2 | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
$rootLBA = [BitConverter]::ToUInt32($buf2, 2)
$rootSize = [BitConverter]::ToUInt32($buf2, 10)
Write-Host "  Root dir LBA: $rootLBA  Size: $rootSize"

$f.Close()
