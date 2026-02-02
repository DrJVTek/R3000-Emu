# Compare R3000 trace (logs/compare_r3000.txt) vs DuckStation reference (logs/compare_duckstation_ref.txt).
# Usage: .\scripts\compare-duckstation.ps1 [-R3000Path <path>] [-RefPath <path>]
# Defaults: -R3000Path logs/compare_r3000.txt -RefPath logs/compare_duckstation_ref.txt

param(
    [string]$R3000Path = "logs/compare_r3000.txt",
    [string]$RefPath = "logs/compare_duckstation_ref.txt"
)

$ErrorActionPreference = "Stop"

function Get-CompareBlocks {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        Write-Error "File not found: $Path"
    }
    $text = Get-Content -Path $Path -Raw
    $blocks = @{}
    $currentPC = $null
    $current = @{}
    foreach ($line in ($text -split "`n")) {
        $line = $line.Trim()
        if ($line -match '^#') { continue }
        if ($line -match '^\[PC=(0x[0-9A-Fa-f]+)\]$') {
            if ($null -ne $currentPC -and $current.Count -gt 0) {
                $blocks[$currentPC] = $current.Clone()
            }
            $currentPC = $Matches[1].ToUpperInvariant()
            $current = @{}
            continue
        }
        # Accept either filled hex (0x...) or blank (key=) for skeletons.
        if ($line -match '^([a-zA-Z0-9_]+)=(.*)$') {
            $key = $Matches[1]
            $val = $Matches[2].Trim()
            if ($val -ne "") {
                $val = $val.ToUpperInvariant()
            }
            $current[$key] = $val
            continue
        }
        if ($line -eq "" -and $null -ne $currentPC -and $current.Count -gt 0) {
            $blocks[$currentPC] = $current.Clone()
            $currentPC = $null
            $current = @{}
        }
    }
    if ($null -ne $currentPC -and $current.Count -gt 0) {
        $blocks[$currentPC] = $current.Clone()
    }
    return $blocks
}

$r3Blocks = Get-CompareBlocks -Path $R3000Path
$refBlocks = Get-CompareBlocks -Path $RefPath

$allPCs = (@($r3Blocks.Keys) + @($refBlocks.Keys) | ForEach-Object { if ($_) { $_.ToUpperInvariant() } } | Where-Object { $_ }) | Sort-Object -Unique
$hasDiff = $false

foreach ($pc in $allPCs) {
    $a = $r3Blocks[$pc]
    $b = $refBlocks[$pc]
    if (-not $a) {
        Write-Host "PC $pc : missing in R3000 trace (only in reference)" -ForegroundColor Yellow
        $hasDiff = $true
        continue
    }
    if (-not $b) {
        Write-Host "PC $pc : missing in DuckStation reference (only in R3000)" -ForegroundColor Yellow
        $hasDiff = $true
        continue
    }
    # Only compare keys which DuckStation filled (non-empty).
    $allKeys = @($a.Keys) + @($b.Keys) | Sort-Object -Unique
    foreach ($k in $allKeys) {
        $va = $a[$k]
        $vb = $b[$k]
        if (-not $vb -or $vb.Trim() -eq "") { continue } # skip unfilled ref fields
        if (-not $va -or $va.Trim() -eq "") { continue }
        $na = [Convert]::ToUInt32($va, 16)
        $nb = [Convert]::ToUInt32($vb, 16)
        if ($na -ne $nb) {
            Write-Host "PC $pc $k : MISMATCH r3000=$va ref=$vb" -ForegroundColor Red
            $hasDiff = $true
        }
    }
}

if (-not $hasDiff) {
    Write-Host "No differences." -ForegroundColor Green
    exit 0
}
exit 1
