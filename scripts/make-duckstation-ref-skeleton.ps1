# Create a blank DuckStation reference file skeleton from logs/compare_r3000.txt
# Usage: .\scripts\make-duckstation-ref-skeleton.ps1 [-R3000Path logs/compare_r3000.txt] [-OutPath logs/compare_duckstation_ref.txt]

param(
    [string]$R3000Path = "logs/compare_r3000.txt",
    [string]$OutPath = "logs/compare_duckstation_ref.txt"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $R3000Path)) {
    throw "File not found: $R3000Path"
}

$text = Get-Content -Path $R3000Path -Raw
$lines = $text -split "`n"

$outDir = Split-Path -Parent $OutPath
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$w = New-Object System.IO.StreamWriter($OutPath, $false, [System.Text.Encoding]::UTF8)
try {
    $w.WriteLine("# DuckStation reference skeleton (fill values from DuckStation debugger)")
    $w.WriteLine("# Based on keys present in: {0}" -f $R3000Path)
    $w.WriteLine("")

    $inBlock = $false
    $pcLine = $null
    $keys = @()

    function Write-Block {
        param([string]$pc, [string[]]$keys)
        if (-not $pc) { return }
        $w.WriteLine($pc.Trim())
        foreach ($k in ($keys | Sort-Object -Unique)) {
            if ($k -match '^[a-zA-Z0-9_]+$') {
                $w.WriteLine("$k=")
            }
        }
        $w.WriteLine("")
    }

    foreach ($raw in $lines) {
        $line = $raw.Trim()
        if ($line -eq "" -and $inBlock) {
            Write-Block -pc $pcLine -keys $keys
            $inBlock = $false
            $pcLine = $null
            $keys = @()
            continue
        }
        if ($line -match '^\[PC=0x[0-9A-Fa-f]+\]$') {
            if ($inBlock) { Write-Block -pc $pcLine -keys $keys }
            $inBlock = $true
            $pcLine = $line.ToUpperInvariant()
            $keys = @()
            continue
        }
        if ($inBlock -and $line -match '^([a-zA-Z0-9_]+)=') {
            $keys += $Matches[1]
        }
    }

    if ($inBlock) {
        Write-Block -pc $pcLine -keys $keys
    }
}
finally {
    $w.Flush()
    $w.Close()
}

Write-Host "Wrote skeleton: $OutPath"
