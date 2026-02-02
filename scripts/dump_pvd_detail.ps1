$f = [System.IO.File]::OpenRead("E:/Projects/PSX/roms/RIDGE RACER (U).BIN")
$pvd = New-Object byte[] 2048
$null = $f.Seek(16 * 2352 + 24, "Begin")
$null = $f.Read($pvd, 0, 2048)
$f.Close()

Write-Host "PVD type: $($pvd[0])  magic: $([System.Text.Encoding]::ASCII.GetString($pvd, 1, 5))"
$ptSize = [BitConverter]::ToUInt32($pvd, 132)
$ptLBA = [BitConverter]::ToUInt32($pvd, 140)
$optPtLBA = [BitConverter]::ToUInt32($pvd, 144)
Write-Host "Path Table Size: $ptSize"
Write-Host "Path Table LBA (Type L): $ptLBA"
Write-Host "Opt Path Table LBA: $optPtLBA"

$rootRecLen = $pvd[156]
$rootLBA = [BitConverter]::ToUInt32($pvd, 158)
$rootSize = [BitConverter]::ToUInt32($pvd, 166)
Write-Host "Root Dir Record Len: $rootRecLen"
Write-Host "Root Dir LBA: $rootLBA"
Write-Host "Root Dir Size: $rootSize"

# Show raw bytes at offsets 132-170
Write-Host "`nPVD raw bytes 132-170:"
$bytes = $pvd[132..170]
Write-Host (($bytes | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
