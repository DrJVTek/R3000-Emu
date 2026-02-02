# Extract compare blocks from logs/io.log into logs/compare_r3000.txt
# Usage: .\scripts\extract-compare-from-iolog.ps1 [-IoLogPath logs/io.log] [-OutPath logs/compare_r3000.txt]

param(
    [string]$IoLogPath = "logs/io.log",
    [string]$OutPath = "logs/compare_r3000.txt"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $IoLogPath)) {
    throw "File not found: $IoLogPath"
}

$lines = Get-Content -Path $IoLogPath

function Write-Block {
    param(
        [System.IO.StreamWriter]$w,
        [string]$pc,
        [hashtable]$kv
    )
    $pcUp = $pc.ToUpperInvariant()
    $w.WriteLine("[PC=$pcUp]")
    foreach ($k in ($kv.Keys | Sort-Object)) {
        $v = $kv[$k]
        if ($null -eq $v) { $v = "" }
        $w.WriteLine("$k=$v")
    }
    $w.WriteLine("")
}

$outDir = Split-Path -Parent $OutPath
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$w = New-Object System.IO.StreamWriter($OutPath, $false, [System.Text.Encoding]::UTF8)
try {
    $w.WriteLine("# R3000 compare trace extracted from logs/io.log")
    $w.WriteLine("# Source: {0}" -f $IoLogPath)
    $w.WriteLine("")

    $cur = $null
    $curPC = $null

    foreach ($raw in $lines) {
        $line = $raw.Trim()

        # loop (PC=0x........)
        if ($line -match 'DBG loop PC=(0x[0-9A-Fa-f]+) (.+)$') {
            if ($curPC -and $cur) { Write-Block -w $w -pc $curPC -kv $cur }
            $curPC = $Matches[1]
            $cur = @{}
            $cur["pc"] = $curPC.ToUpperInvariant()
            $rest = $Matches[2]
            foreach ($pair in ($rest -split ' ')) {
                if ($pair -match '^([a-z0-9]+)=(0x[0-9A-Fa-f]+)$') {
                    $cur[$Matches[1]] = $Matches[2].ToUpperInvariant()
                }
            }
            continue
        }

        if ($line -match 'DBG globals: \[0x8009A204\]=(0x[0-9A-Fa-f]+) \*\(that\)=(0x[0-9A-Fa-f]+)') {
            if ($cur) {
                $cur["global_8009A204"] = $Matches[1].ToUpperInvariant()
                $cur["global_ptr_val"] = $Matches[2].ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop2 PC=(0x[0-9A-Fa-f]+) (.+)$') {
            if ($curPC -and $cur) { Write-Block -w $w -pc $curPC -kv $cur }
            $curPC = $Matches[1]
            $cur = @{}
            $cur["pc"] = $curPC.ToUpperInvariant()
            foreach ($pair in (($Matches[2]) -split ' ')) {
                if ($pair -match '^([a-z0-9_]+)=(0x[0-9A-Fa-f]+)$') {
                    $cur[$Matches[1]] = $Matches[2].ToUpperInvariant()
                }
            }
            continue
        }

        if ($line -match 'DBG loop2 IRQ: I_STAT=(0x[0-9A-Fa-f]+) I_MASK=(0x[0-9A-Fa-f]+) pend=(0x[0-9A-Fa-f]+)') {
            if ($cur) {
                $cur["i_stat"] = $Matches[1].ToUpperInvariant()
                $cur["i_mask"] = $Matches[2].ToUpperInvariant()
                $cur["pend"] = $Matches[3].ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop2 DMA2: MADR=(0x[0-9A-Fa-f]+) BCR=(0x[0-9A-Fa-f]+) CHCR=(0x[0-9A-Fa-f]+) DPCR=(0x[0-9A-Fa-f]+) DICR=(0x[0-9A-Fa-f]+)') {
            if ($cur) {
                $cur["dma2_madr"] = $Matches[1].ToUpperInvariant()
                $cur["dma2_bcr"] = $Matches[2].ToUpperInvariant()
                $cur["dma2_chcr"] = $Matches[3].ToUpperInvariant()
                $cur["dpcr"] = $Matches[4].ToUpperInvariant()
                $cur["dicr"] = $Matches[5].ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop2 TMR: t0\(c=([0-9A-Fa-f]+) m=([0-9A-Fa-f]+) t=([0-9A-Fa-f]+)\) t1\(c=([0-9A-Fa-f]+) m=([0-9A-Fa-f]+) t=([0-9A-Fa-f]+)\) t2\(c=([0-9A-Fa-f]+) m=([0-9A-Fa-f]+) t=([0-9A-Fa-f]+)\)') {
            if ($cur) {
                $cur["t0c"] = ("0x" + $Matches[1]).ToUpperInvariant()
                $cur["t0m"] = ("0x" + $Matches[2]).ToUpperInvariant()
                $cur["t0t"] = ("0x" + $Matches[3]).ToUpperInvariant()
                $cur["t1c"] = ("0x" + $Matches[4]).ToUpperInvariant()
                $cur["t1m"] = ("0x" + $Matches[5]).ToUpperInvariant()
                $cur["t1t"] = ("0x" + $Matches[6]).ToUpperInvariant()
                $cur["t2c"] = ("0x" + $Matches[7]).ToUpperInvariant()
                $cur["t2m"] = ("0x" + $Matches[8]).ToUpperInvariant()
                $cur["t2t"] = ("0x" + $Matches[9]).ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop5 PC=(0x[0-9A-Fa-f]+) (.+)$') {
            if ($curPC -and $cur) { Write-Block -w $w -pc $curPC -kv $cur }
            $curPC = $Matches[1]
            $cur = @{}
            $cur["pc"] = $curPC.ToUpperInvariant()
            foreach ($pair in (($Matches[2]) -split ' ')) {
                if ($pair -match '^([a-z0-9_]+)=(0x[0-9A-Fa-f]+)$') {
                    $cur[$Matches[1]] = $Matches[2].ToUpperInvariant()
                }
            }
            continue
        }

        if ($line -match 'DBG loop5 IRQ: I_STAT=(0x[0-9A-Fa-f]+) I_MASK=(0x[0-9A-Fa-f]+) pend=(0x[0-9A-Fa-f]+)') {
            if ($cur) {
                $cur["i_stat"] = $Matches[1].ToUpperInvariant()
                $cur["i_mask"] = $Matches[2].ToUpperInvariant()
                $cur["pend"] = $Matches[3].ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop5 GPUSTAT=(0x[0-9A-Fa-f]+) DPCR=(0x[0-9A-Fa-f]+) DICR=(0x[0-9A-Fa-f]+)') {
            if ($cur) {
                $cur["gpustat"] = $Matches[1].ToUpperInvariant()
                $cur["dpcr"] = $Matches[2].ToUpperInvariant()
                $cur["dicr"] = $Matches[3].ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop5 TMR: t0\(c=([0-9A-Fa-f]+) m=([0-9A-Fa-f]+) t=([0-9A-Fa-f]+)\) t1\(c=([0-9A-Fa-f]+) m=([0-9A-Fa-f]+) t=([0-9A-Fa-f]+)\) t2\(c=([0-9A-Fa-f]+) m=([0-9A-Fa-f]+) t=([0-9A-Fa-f]+)\)') {
            if ($cur) {
                $cur["t0c"] = ("0x" + $Matches[1]).ToUpperInvariant()
                $cur["t0m"] = ("0x" + $Matches[2]).ToUpperInvariant()
                $cur["t0t"] = ("0x" + $Matches[3]).ToUpperInvariant()
                $cur["t1c"] = ("0x" + $Matches[4]).ToUpperInvariant()
                $cur["t1m"] = ("0x" + $Matches[5]).ToUpperInvariant()
                $cur["t1t"] = ("0x" + $Matches[6]).ToUpperInvariant()
                $cur["t2c"] = ("0x" + $Matches[7]).ToUpperInvariant()
                $cur["t2m"] = ("0x" + $Matches[8]).ToUpperInvariant()
                $cur["t2t"] = ("0x" + $Matches[9]).ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop6 PC=(0x[0-9A-Fa-f]+) (.+)$') {
            if ($curPC -and $cur) { Write-Block -w $w -pc $curPC -kv $cur }
            $curPC = $Matches[1]
            $cur = @{}
            $cur["pc"] = $curPC.ToUpperInvariant()
            foreach ($pair in (($Matches[2]) -split ' ')) {
                if ($pair -match '^([a-z0-9_]+)=(0x[0-9A-Fa-f]+)$') {
                    $cur[$Matches[1]] = $Matches[2].ToUpperInvariant()
                }
            }
            continue
        }

        if ($line -match 'DBG loop6 IRQ: I_STAT=(0x[0-9A-Fa-f]+) I_MASK=(0x[0-9A-Fa-f]+) pend=(0x[0-9A-Fa-f]+)') {
            if ($cur) {
                $cur["i_stat"] = $Matches[1].ToUpperInvariant()
                $cur["i_mask"] = $Matches[2].ToUpperInvariant()
                $cur["pend"] = $Matches[3].ToUpperInvariant()
            }
            continue
        }

        if ($line -match 'DBG loop6 GPUSTAT=(0x[0-9A-Fa-f]+) DPCR=(0x[0-9A-Fa-f]+) DICR=(0x[0-9A-Fa-f]+) MMIO10F8=([0-9A-Fa-f]+) MMIO10FC=([0-9A-Fa-f]+) MMIO1D80=([0-9A-Fa-f]+) MMIO1D84=([0-9A-Fa-f]+)') {
            if ($cur) {
                $cur["gpustat"] = $Matches[1].ToUpperInvariant()
                $cur["dpcr"] = $Matches[2].ToUpperInvariant()
                $cur["dicr"] = $Matches[3].ToUpperInvariant()
                $cur["mmio_10f8"] = ("0x" + $Matches[4]).ToUpperInvariant()
                $cur["mmio_10fc"] = ("0x" + $Matches[5]).ToUpperInvariant()
                $cur["mmio_1d80"] = ("0x" + $Matches[6]).ToUpperInvariant()
                $cur["mmio_1d84"] = ("0x" + $Matches[7]).ToUpperInvariant()
            }
            continue
        }

        # instruction lines (examples: "DBG ins0-7 :" , "DBG loop6 ins16-23:")
        if ($line -match 'DBG .*ins([0-9]+)-([0-9]+)\s*:\s*(.+)$') {
            if ($cur) {
                $start = [int]$Matches[1]
                $payload = $Matches[3].Trim()
                $hexs = ($payload -split ' ') | Where-Object { $_ -match '^[0-9A-Fa-f]{8}$' }

                for ($i = 0; $i -lt $hexs.Count; $i++) {
                    $idx = $start + $i
                    $cur[("ins{0}" -f $idx)] = ("0x" + $hexs[$i]).ToUpperInvariant()
                }
            }
            continue
        }
    }

    if ($curPC -and $cur) { Write-Block -w $w -pc $curPC -kv $cur }
}
finally {
    $w.Flush()
    $w.Close()
}

Write-Host "Wrote: $OutPath"
