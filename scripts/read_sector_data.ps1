$binFile = 'E:/Projects/PSX/roms/Ridge Racer (U).bin'
$lba = 236
$sectorSize = 2352
$dataOffset = 24
$fcbOffset = 0x3E4  # 996

$fileOffset = $lba * $sectorSize + $dataOffset + $fcbOffset
$bytes = [System.IO.File]::ReadAllBytes($binFile)

Write-Host "Sector LBA $lba, offset 0x3E4 (996) in file at 0x$($fileOffset.ToString('X')):"

$hex = ""
for ($i = 0; $i -lt 16; $i++) {
    $hex += "{0:X2} " -f $bytes[$fileOffset + $i]
}
Write-Host $hex

# Also check what 0x48 0x18 0x38 looks like
Write-Host "`nSearching for 48 18 38 pattern..."
