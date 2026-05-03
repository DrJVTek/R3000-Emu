param(
    [string]$PluginRoot = "E:\Projects\github\Live\PSXVR\Plugins\R3000Emu",
    [string]$ProcessName = "UnrealEditor"
)

$ErrorActionPreference = "Stop"

function Get-AsciiText {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    return [System.Text.Encoding]::ASCII.GetString($bytes)
}

function Get-MarkerInfo {
    param([string]$Path)

    $text = Get-AsciiText -Path $Path
    $markers = [regex]::Matches($text, 'BUS source v[0-9]+[^"\x00\r\n]*') |
        ForEach-Object { $_.Value } |
        Sort-Object -Unique

    [PSCustomObject]@{
        Path = $Path
        LastWrite = (Get-Item -LiteralPath $Path).LastWriteTime
        Size = (Get-Item -LiteralPath $Path).Length
        BusMarkers = if ($markers) { ($markers -join "; ") } else { "" }
        HasPadPhase = $text.Contains("SIO0_PAD_PHASE")
        HasPadRead = $text.Contains("SIO0_PAD_READ")
        HasPadAck = $text.Contains("SIO0_PAD_ACK")
        HasSio0Mmio = $text.Contains("SIO0_MMIO")
    }
}

$modulesPath = Join-Path $PluginRoot "Binaries\Win64\UnrealEditor.modules"
if (Test-Path -LiteralPath $modulesPath) {
    $modulesJson = Get-Content -LiteralPath $modulesPath -Raw | ConvertFrom-Json
    Write-Host "Plugin modules file: $modulesPath"
    Write-Host "Plugin BuildId: $($modulesJson.BuildId)"
    Write-Host ""
}
else {
    Write-Host "Plugin modules file not found: $modulesPath"
    Write-Host ""
}

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $proc) {
    Write-Host "Process not running: $ProcessName"
    exit 0
}

Write-Host "Process: $ProcessName pid=$($proc.Id)"

$loaded = $proc.Modules |
    Where-Object { $_.FileName -like "*R3000EmuRuntime*" } |
    Sort-Object FileName

if (-not $loaded) {
    Write-Host "No R3000EmuRuntime modules loaded."
    exit 0
}

$rows = foreach ($module in $loaded) {
    Get-MarkerInfo -Path $module.FileName
}

$rows | Format-Table -AutoSize | Out-String -Width 4096
