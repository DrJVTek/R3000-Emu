# Run BIOS boot with HLE for longer to see if it gets past initialization
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $root "build_dbg\Debug\r3000_emu.exe"
$bios = Join-Path $root "bios\ps1_bios.bin"
$cd = Join-Path $root "ridgeracer.cue"

$args_str = "--bios=$bios --cd=$cd --hle --max-steps=100000000 --pc-sample=20000000 --emu-log-level=info"
Write-Host "Running: $args_str"

$p = Start-Process -FilePath $exe `
    -ArgumentList $args_str `
    -WorkingDirectory $root `
    -RedirectStandardOutput (Join-Path $root "stdout_bioslong.txt") `
    -RedirectStandardError (Join-Path $root "stderr_bioslong.txt") `
    -PassThru -NoNewWindow

# Wait up to 3 minutes
$timeout = 180
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $timeout) {
    Start-Sleep -Seconds 5
    Write-Host "Running... ($([int]$sw.Elapsed.TotalSeconds)s)"
}
if (-not $p.HasExited) {
    Write-Host "Timeout - killing process"
    $p.Kill()
}

Write-Host ""
Write-Host "=== SAMPLES ==="
Select-String -Path (Join-Path $root "stderr_bioslong.txt") -Pattern "SAMPLE" | Select-Object -ExpandProperty Line
Write-Host ""
Write-Host "=== CD Commands ==="
Select-String -Path (Join-Path $root "stderr_bioslong.txt") -Pattern "CMD 0x02|CMD 0x06|SETLOC" | Select-Object -First 20 -ExpandProperty Line
Write-Host ""
Write-Host "=== LAST 20 ==="
Get-Content (Join-Path $root "stderr_bioslong.txt") | Select-Object -Last 20
