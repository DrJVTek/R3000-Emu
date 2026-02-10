# Read PVD and Path Table from the BIN file
$binPath = 'E:\Projects\PSX\roms\Ridge Racer (U).bin'
if (Test-Path $binPath) {
    $fs = [System.IO.File]::OpenRead($binPath)

    # Read sector 16 (PVD)
    $null = $fs.Seek(16 * 2352, [System.IO.SeekOrigin]::Begin)
    $sec16 = New-Object byte[] 2352
    $null = $fs.Read($sec16, 0, 2352)

    Write-Host "=== Sector 16 (PVD) ==="
    Write-Host ""

    # PVD fields (user data starts at byte 24 for Mode 2)
    $ud = 24

    Write-Host "Volume Descriptor Type: $($sec16[$ud + 0])"
    Write-Host ("Standard ID: {0}{1}{2}{3}{4}" -f [char]$sec16[$ud + 1], [char]$sec16[$ud + 2], [char]$sec16[$ud + 3], [char]$sec16[$ud + 4], [char]$sec16[$ud + 5])
    Write-Host "Version: $($sec16[$ud + 6])"

    # Path Table Size (bytes 132-139)
    $pathTableSize = [BitConverter]::ToUInt32($sec16, $ud + 132)
    Write-Host "Path Table Size: $pathTableSize bytes"

    # L Path Table Location (bytes 140-143)
    $lPathTable = [BitConverter]::ToUInt32($sec16, $ud + 140)
    Write-Host "L Path Table LBA: $lPathTable"

    # M Path Table Location (bytes 148-151, big-endian)
    $mPathTable = ([uint32]$sec16[$ud + 148] -shl 24) -bor ([uint32]$sec16[$ud + 149] -shl 16) -bor ([uint32]$sec16[$ud + 150] -shl 8) -bor $sec16[$ud + 151]
    Write-Host "M Path Table LBA (BE): $mPathTable"

    # Root Directory Record (bytes 156-189)
    Write-Host "`n=== Root Directory Record (at offset 156) ==="
    $rd = $ud + 156
    Write-Host "Record Length: $($sec16[$rd])"
    $rootLba = [BitConverter]::ToUInt32($sec16, $rd + 2)
    Write-Host "Root Dir LBA: $rootLba"
    $rootSize = [BitConverter]::ToUInt32($sec16, $rd + 10)
    Write-Host "Root Dir Size: $rootSize bytes"

    # Read L Path Table
    Write-Host "`n=== L Path Table (sector $lPathTable) ==="
    $null = $fs.Seek($lPathTable * 2352 + 24, [System.IO.SeekOrigin]::Begin)
    $pathTable = New-Object byte[] 2048
    $null = $fs.Read($pathTable, 0, 2048)

    # Parse first few path table entries
    $pos = 0
    $entryNum = 1
    while ($pos -lt $pathTableSize -and $entryNum -le 10) {
        $nameLen = $pathTable[$pos]
        if ($nameLen -eq 0) { break }
        $extAttr = $pathTable[$pos + 1]
        $extentLba = [BitConverter]::ToUInt32($pathTable, $pos + 2)
        $parentDir = [BitConverter]::ToUInt16($pathTable, $pos + 6)
        $name = ""
        for ($i = 0; $i -lt $nameLen; $i++) {
            $name += [char]$pathTable[$pos + 8 + $i]
        }
        Write-Host ("Entry {0}: Name='{1}' LBA={2} Parent={3}" -f $entryNum, $name, $extentLba, $parentDir)
        $pos += 8 + $nameLen
        if ($nameLen % 2 -eq 1) { $pos++ }  # padding
        $entryNum++
    }

    # Read Root Directory (sector $rootLba)
    Write-Host "`n=== Root Directory (sector $rootLba) ==="
    $null = $fs.Seek($rootLba * 2352 + 24, [System.IO.SeekOrigin]::Begin)
    $rootDir = New-Object byte[] 2048
    $null = $fs.Read($rootDir, 0, 2048)

    # Parse first few directory entries
    $pos = 0
    $entryNum = 1
    while ($pos -lt 2048 -and $entryNum -le 15) {
        $recLen = $rootDir[$pos]
        if ($recLen -eq 0) { break }
        $extAttr = $rootDir[$pos + 1]
        $extentLba = [BitConverter]::ToUInt32($rootDir, $pos + 2)
        $dataLen = [BitConverter]::ToUInt32($rootDir, $pos + 10)
        $flags = $rootDir[$pos + 25]
        $nameLen = $rootDir[$pos + 32]
        $name = ""
        for ($i = 0; $i -lt $nameLen; $i++) {
            $c = $rootDir[$pos + 33 + $i]
            if ($c -eq 0) { $name += "." }
            elseif ($c -eq 1) { $name += ".." }
            else { $name += [char]$c }
        }
        $isDir = if ($flags -band 2) { "DIR" } else { "FILE" }
        Write-Host ("{0}: '{1}' LBA={2} Size={3} [{4}]" -f $entryNum, $name, $extentLba, $dataLen, $isDir)

        # Check for suspicious values
        if ($extentLba -gt 200000) {
            Write-Host "  ** WARNING: LBA $extentLba > disc size!"
        }

        $pos += $recLen
        $entryNum++
    }

    $fs.Close()
} else {
    Write-Host "BIN file not found: $binPath"
}
